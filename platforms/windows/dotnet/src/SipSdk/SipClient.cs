using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;
using SipSdk.Native;

namespace SipSdk
{
    /// <summary>
    /// High-level client managing the SIP VoIP engine lifecycle, account registration,
    /// and outgoing/incoming call orchestration.
    /// </summary>
    public sealed class SipClient : IDisposable
    {
        private IntPtr _clientHandle;
        private readonly SynchronizationContext? _syncContext;
        private readonly ConcurrentDictionary<IntPtr, SipCall> _activeCalls = new();
        private readonly ConcurrentDictionary<IntPtr, SipAccount> _accounts = new();

        // Pinned delegate references to prevent GC collection of native callback pointers
        private readonly NativeRegStateCallback _regCallback;
        private readonly NativeIncomingCallCallback _incomingCallCallback;
        private readonly NativeCallStateCallback _callStateCallback;
        private readonly NativeCallMediaCallback _callMediaCallback;
        private readonly NativeDtmfDigitCallback _dtmfCallback;
        private readonly NativeCallStatsCallback _callStatsCallback;
        private readonly NativeTransferStatusCallback _transferStatusCallback;
        private readonly NativeTransferRequestCallback _transferRequestCallback;
        private readonly NativeCallRejectedBusyCallback _callRejectedBusyCallback;
        private readonly NativeConferenceChangedCallback _conferenceChangedCallback;
        private readonly NativeMessageReceivedCallback _messageReceivedCallback;
        private readonly NativeMessageStatusCallback _messageStatusCallback;
        private readonly SipNativeAbi.NativeRecordingStateCallback _recordingStateCallback;
        private readonly NativeAccountRegStateCallback _accountRegStateCallback;
        private readonly NativeAccountMessageReceivedCallback _accountMessageReceivedCallback;

        private GCHandle _gcHandle;
        private bool _disposed;
        private string _accountAor = string.Empty;

        private int _maxCalls = 4;
        public int MaxCalls
        {
            get => _maxCalls;
            set => _maxCalls = Math.Max(1, Math.Min(8, value));
        }
        public bool IsRegistered => _clientHandle != IntPtr.Zero && SipNativeAbi.sip_client_is_registered(_clientHandle);
        public bool IsMicMuted => _clientHandle != IntPtr.Zero && SipNativeAbi.sip_client_is_mic_muted(_clientHandle);
        public string AccountAor => _accountAor;

        public SipAccount? DefaultAccount
        {
            get
            {
                if (_clientHandle == IntPtr.Zero) return null;
                IntPtr handle = SipNativeAbi.sip_client_get_default_account(_clientHandle);
                return ResolveAccount(handle);
            }
        }

        public IReadOnlyList<SipAccount> Accounts
        {
            get
            {
                if (_clientHandle == IntPtr.Zero) return Array.Empty<SipAccount>();
                int count = (int)SipNativeAbi.sip_client_get_account_count(_clientHandle);
                if (count <= 0) return Array.Empty<SipAccount>();
                IntPtr[] handles = new IntPtr[count];
                int written = (int)SipNativeAbi.sip_client_get_accounts(_clientHandle, handles, (UIntPtr)count);
                var list = new List<SipAccount>(written);
                for (int i = 0; i < written; i++)
                {
                    var acc = ResolveAccount(handles[i]);
                    if (acc != null) list.Add(acc);
                }
                return list;
            }
        }

        public event EventHandler<RegistrationStateChangedEventArgs>? RegistrationStateChanged;
        public event EventHandler<IncomingCallEventArgs>? IncomingCall;
        public event EventHandler<CallRejectedBusyEventArgs>? CallRejectedBusy;
        public event EventHandler<ConferenceChangedEventArgs>? ConferenceChanged;
        public event EventHandler<MessageReceivedEventArgs>? MessageReceived;
        public event EventHandler<MessageStatusChangedEventArgs>? MessageStatusChanged;
        public event EventHandler<RecordingStateChangedEventArgs>? RecordingStateChanged;
        public event EventHandler<SipAccountRegistrationStateEventArgs>? AccountRegistrationStateChanged;
        public event EventHandler<SipAccountMessageReceivedEventArgs>? AccountMessageReceived;

        public SipClient()
        {
            _syncContext = SynchronizationContext.Current;

            _regCallback = HandleNativeRegState;
            _incomingCallCallback = HandleNativeIncomingCall;
            _callStateCallback = HandleNativeCallState;
            _callMediaCallback = HandleNativeCallMedia;
            _dtmfCallback = HandleNativeDtmfDigit;
            _callStatsCallback = HandleNativeCallStats;
            _transferStatusCallback = HandleNativeTransferStatus;
            _transferRequestCallback = HandleNativeTransferRequest;
            _callRejectedBusyCallback = HandleNativeCallRejectedBusy;
            _conferenceChangedCallback = HandleNativeConferenceChanged;
            _messageReceivedCallback = HandleNativeMessageReceived;
            _messageStatusCallback = HandleNativeMessageStatus;
            _recordingStateCallback = HandleNativeRecordingState;
            _accountRegStateCallback = HandleNativeAccountRegState;
            _accountMessageReceivedCallback = HandleNativeAccountMessageReceived;

            _gcHandle = GCHandle.Alloc(this);
            IntPtr userData = GCHandle.ToIntPtr(_gcHandle);

            var callbacks = new SipCallbacksNative
            {
                OnRegState = Marshal.GetFunctionPointerForDelegate(_regCallback),
                OnIncomingCall = Marshal.GetFunctionPointerForDelegate(_incomingCallCallback),
                OnCallState = Marshal.GetFunctionPointerForDelegate(_callStateCallback),
                OnCallMedia = Marshal.GetFunctionPointerForDelegate(_callMediaCallback),
                OnDtmfDigit = Marshal.GetFunctionPointerForDelegate(_dtmfCallback),
                OnCallStats = Marshal.GetFunctionPointerForDelegate(_callStatsCallback),
                OnTransferStatus = Marshal.GetFunctionPointerForDelegate(_transferStatusCallback),
                OnTransferRequest = Marshal.GetFunctionPointerForDelegate(_transferRequestCallback),
                OnCallRejectedBusy = Marshal.GetFunctionPointerForDelegate(_callRejectedBusyCallback),
                OnConferenceChanged = Marshal.GetFunctionPointerForDelegate(_conferenceChangedCallback)
            };

            _clientHandle = SipNativeAbi.sip_client_create(ref callbacks, userData);
            if (_clientHandle == IntPtr.Zero)
            {
                throw new InvalidOperationException("Failed to allocate native SIP client handle.");
            }

            var messageCallbacks = new SipMessageCallbacksNative
            {
                OnMessageReceived = Marshal.GetFunctionPointerForDelegate(_messageReceivedCallback),
                OnMessageStatus = Marshal.GetFunctionPointerForDelegate(_messageStatusCallback)
            };
            SipNativeAbi.sip_client_set_message_callbacks(_clientHandle, ref messageCallbacks);

            var recordingCallbacks = new SipNativeAbi.SipRecordingCallbacksNative
            {
                OnRecordingState = Marshal.GetFunctionPointerForDelegate(_recordingStateCallback)
            };
            SipNativeAbi.sip_client_set_recording_callbacks(_clientHandle, ref recordingCallbacks);

            var accountCallbacks = new SipAccountCallbacks
            {
                OnAccountRegState = Marshal.GetFunctionPointerForDelegate(_accountRegStateCallback),
                OnAccountMessageReceived = Marshal.GetFunctionPointerForDelegate(_accountMessageReceivedCallback)
            };
            SipNativeAbi.sip_client_set_account_callbacks(_clientHandle, ref accountCallbacks);
        }

        public int Start(string? configPath = null)
        {
            ThrowIfDisposed();
            return SipNativeAbi.sip_client_init(_clientHandle, configPath);
        }

        public void Stop()
        {
            if (_clientHandle != IntPtr.Zero)
            {
                SipNativeAbi.sip_client_destroy(_clientHandle);
                _clientHandle = IntPtr.Zero;
            }
        }

        public int Register(
            string username,
            string domain,
            string password,
            string transport = "udp",
            int port = 5060,
            string? tlsCaFile = null,
            bool verifyTlsCert = true,
            string? mediaEnc = null,
            int? maxCalls = null)
        {
            ThrowIfDisposed();
            if (string.IsNullOrEmpty(username)) throw new ArgumentException("Username cannot be empty", nameof(username));
            if (string.IsNullOrEmpty(domain)) throw new ArgumentException("Domain cannot be empty", nameof(domain));

            if (maxCalls.HasValue)
            {
                MaxCalls = maxCalls.Value;
            }

            _accountAor = $"sip:{username}@{domain}:{port}";
            var cfg = new SipNativeAbi.SipAccountConfigNative
            {
                User = username,
                Domain = domain,
                Pwd = password,
                Transport = transport,
                Port = port,
                TlsCaFile = tlsCaFile,
                VerifyTlsCert = verifyTlsCert,
                MediaEnc = mediaEnc,
                MaxCalls = MaxCalls
            };
            return SipNativeAbi.sip_client_register_account(_clientHandle, ref cfg);
        }

        public int Unregister()
        {
            ThrowIfDisposed();
            return SipNativeAbi.sip_client_unregister(_clientHandle);
        }

        public SipAccount AddAccount(SipAccountConfig config)
        {
            ThrowIfDisposed();
            if (config == null) throw new ArgumentNullException(nameof(config));
            if (string.IsNullOrEmpty(config.Username)) throw new ArgumentException("Username cannot be empty", nameof(config));
            if (string.IsNullOrEmpty(config.Domain)) throw new ArgumentException("Domain cannot be empty", nameof(config));

            int maxCalls = config.MaxCalls > 0 ? config.MaxCalls : MaxCalls;
            var nativeCfg = new SipNativeAbi.SipAccountConfigNative
            {
                User = config.Username,
                Domain = config.Domain,
                Pwd = config.Password,
                Transport = string.IsNullOrEmpty(config.Transport) ? "udp" : config.Transport,
                Port = config.Port,
                DisplayName = config.DisplayName,
                OutboundProxy = config.OutboundProxy,
                StunServer = config.StunServer,
                StunUser = config.StunUser,
                StunPass = config.StunPass,
                MediaNat = config.MediaNat,
                RegInterval = config.RegInterval,
                TlsCaFile = config.TlsCaFile,
                VerifyTlsCert = config.VerifyTlsCert,
                MediaEnc = config.MediaEnc,
                MaxCalls = maxCalls
            };

            int rc = SipNativeAbi.sip_client_add_account(_clientHandle, ref nativeCfg, out IntPtr accountHandle);
            if (rc == 16)
            {
                throw new SipBusyException("Maximum concurrent accounts reached (8).");
            }
            if (rc == 17)
            {
                throw new InvalidOperationException($"Account {config.Username}@{config.Domain} is already added (EEXIST).");
            }
            if (rc != 0 || accountHandle == IntPtr.Zero)
            {
                throw new InvalidOperationException($"Failed to add account {config.Username}@{config.Domain} (error {rc}).");
            }

            var account = _accounts.GetOrAdd(accountHandle, h => new SipAccount(h, this, ownsHandle: true));
            account.PromoteToOwned();
            return account;
        }

        public SipAccount AddAccount(
            string username,
            string domain,
            string password,
            string transport = "udp",
            int port = 5060,
            string? tlsCaFile = null,
            bool verifyTlsCert = true,
            string? mediaEnc = null,
            int? maxCalls = null)
        {
            if (maxCalls.HasValue)
            {
                MaxCalls = maxCalls.Value;
            }

            return AddAccount(new SipAccountConfig
            {
                Username = username,
                Domain = domain,
                Password = password,
                Transport = transport,
                Port = port,
                TlsCaFile = tlsCaFile,
                VerifyTlsCert = verifyTlsCert,
                MediaEnc = mediaEnc,
                MaxCalls = MaxCalls
            });
        }

        public void RemoveAccount(SipAccount account)
        {
            ThrowIfDisposed();
            if (account == null) throw new ArgumentNullException(nameof(account));
            account.ThrowIfDisposed();

            int rc = SipNativeAbi.sip_client_remove_account(_clientHandle, account.NativeHandle);
            if (rc != 0)
            {
                throw new InvalidOperationException($"Failed to remove account {account.Aor} (error {rc}).");
            }
        }

        public void SetDefaultAccount(SipAccount account)
        {
            ThrowIfDisposed();
            if (account == null) throw new ArgumentNullException(nameof(account));
            account.ThrowIfDisposed();

            int rc = SipNativeAbi.sip_client_set_default_account(_clientHandle, account.NativeHandle);
            if (rc != 0)
            {
                throw new InvalidOperationException($"Failed to set default account {account.Aor} (error {rc}).");
            }
        }

        public SipCall Dial(string targetUri, bool withVideo = false)
        {
            ThrowIfDisposed();
            if (string.IsNullOrEmpty(targetUri)) throw new ArgumentException("Target URI cannot be empty", nameof(targetUri));

            int rc = SipNativeAbi.sip_call_dial(_clientHandle, targetUri, withVideo, out IntPtr callHandle);
            if (rc == 16)
            {
                throw new SipBusyException($"Maximum concurrent calls reached ({MaxCalls})");
            }
            if (rc != 0 || callHandle == IntPtr.Zero)
            {
                throw new InvalidOperationException($"Failed to initiate call to {targetUri} (error {rc})");
            }

            var call = new SipCall(callHandle, this);
            _activeCalls[callHandle] = call;
            return call;
        }

        public SipCall MakeCall(string targetUri, bool withVideo = false) => Dial(targetUri, withVideo);

        public void MergeCalls(SipCall callA, SipCall callB)
        {
            ThrowIfDisposed();
            if (callA == null || callB == null) throw new ArgumentNullException(callA == null ? nameof(callA) : nameof(callB));
            int rc = SipNativeAbi.sip_client_merge_calls(_clientHandle, callA.NativeHandle, callB.NativeHandle);
            if (rc != 0) throw new InvalidOperationException($"sip_client_merge_calls failed with code {rc}");
        }

        public void AddToConference(SipCall call)
        {
            ThrowIfDisposed();
            if (call == null) throw new ArgumentNullException(nameof(call));
            int rc = SipNativeAbi.sip_client_add_to_conference(_clientHandle, call.NativeHandle);
            if (rc != 0) throw new InvalidOperationException($"sip_client_add_to_conference failed with code {rc}");
        }

        public void SplitCall(SipCall call)
        {
            ThrowIfDisposed();
            if (call == null) throw new ArgumentNullException(nameof(call));
            int rc = SipNativeAbi.sip_client_split_call(_clientHandle, call.NativeHandle);
            if (rc != 0) throw new InvalidOperationException($"sip_client_split_call failed with code {rc}");
        }

        public void LeaveConference()
        {
            ThrowIfDisposed();
            int rc = SipNativeAbi.sip_client_leave_conference(_clientHandle);
            if (rc != 0) throw new InvalidOperationException($"sip_client_leave_conference failed with code {rc}");
        }

        /// <summary>
        /// Sends a UTF-8 text message (SIP MESSAGE, text/plain).
        /// Returns the local message id; its result arrives through <see cref="MessageStatusChanged"/>.
        /// </summary>
        public ulong SendMessage(string targetUri, string text)
        {
            if (string.IsNullOrEmpty(text)) throw new ArgumentException("Message text cannot be empty", nameof(text));
            return SendMessage(targetUri, System.Text.Encoding.UTF8.GetBytes(text), "text/plain");
        }

        /// <summary>
        /// Sends a SIP MESSAGE with any MIME content type (e.g. "application/json").
        /// Returns the local message id; its result arrives through <see cref="MessageStatusChanged"/>.
        /// </summary>
        public ulong SendMessage(string targetUri, byte[] body, string contentType)
        {
            ThrowIfDisposed();
            if (string.IsNullOrEmpty(targetUri)) throw new ArgumentException("Target URI cannot be empty", nameof(targetUri));
            if (body == null || body.Length == 0) throw new ArgumentException("Message body cannot be empty", nameof(body));
            if (string.IsNullOrEmpty(contentType)) throw new ArgumentException("Content type cannot be empty", nameof(contentType));

            int rc = SipNativeAbi.sip_client_send_message(
                _clientHandle,
                SipNativeAbi.ToUtf8Z(targetUri),
                SipNativeAbi.ToUtf8Z(contentType),
                body,
                (UIntPtr)body.Length,
                out ulong messageId);
            if (rc != 0)
            {
                throw new InvalidOperationException($"Failed to send message to {targetUri} (error {rc})");
            }
            return messageId;
        }

        public void SetMicMuted(bool muted)
        {
            ThrowIfDisposed();
            SipNativeAbi.sip_client_set_mic_muted(_clientHandle, muted);
        }

        public void SwitchCamera()
        {
            ThrowIfDisposed();
            SipNativeAbi.sip_client_switch_camera(_clientHandle);
        }

        public void SetCamera(int position)
        {
            ThrowIfDisposed();
            SipNativeAbi.sip_client_set_camera(_clientHandle, position);
        }

        public void SetLocalVideoEnabled(bool enabled)
        {
            ThrowIfDisposed();
            SipNativeAbi.sip_client_set_local_video_enabled(_clientHandle, enabled);
        }

        public bool IsLocalVideoEnabled()
        {
            ThrowIfDisposed();
            return SipNativeAbi.sip_client_is_local_video_enabled(_clientHandle);
        }

        public int StartLocalPreview()
        {
            ThrowIfDisposed();
            return SipNativeAbi.sip_client_start_local_preview(_clientHandle);
        }

        public int StopLocalPreview()
        {
            ThrowIfDisposed();
            return SipNativeAbi.sip_client_stop_local_preview(_clientHandle);
        }

        public bool IsLocalPreviewActive()
        {
            ThrowIfDisposed();
            return SipNativeAbi.sip_client_is_local_preview_active(_clientHandle);
        }

        internal SipCall RegisterCall(IntPtr callHandle)
        {
            var call = new SipCall(callHandle, this);
            _activeCalls[callHandle] = call;
            return call;
        }

        internal void RemoveCall(IntPtr callHandle)
        {
            _activeCalls.TryRemove(callHandle, out _);
        }

        internal SipAccount? ResolveAccount(IntPtr accountHandle)
        {
            if (accountHandle == IntPtr.Zero) return null;
            if (_accounts.TryGetValue(accountHandle, out var existing))
            {
                return existing;
            }
            bool isActive = false;
            if (_clientHandle != IntPtr.Zero)
            {
                nuint count = SipNativeAbi.sip_client_get_account_count(_clientHandle);
                if (count > 0)
                {
                    var handles = new IntPtr[(int)count];
                    nuint written = SipNativeAbi.sip_client_get_accounts(_clientHandle, handles, count);
                    for (int i = 0; i < (int)written; i++)
                    {
                        if (handles[i] == accountHandle)
                        {
                            isActive = true;
                            break;
                        }
                    }
                }
            }
            if (isActive)
            {
                return _accounts.GetOrAdd(accountHandle, h => new SipAccount(h, this, ownsHandle: false));
            }
            var transient = new SipAccount(accountHandle, this, ownsHandle: false);
            transient.DetachHandle();
            return transient;
        }

        internal void RemoveAccountEntry(IntPtr accountHandle)
        {
            _accounts.TryRemove(accountHandle, out _);
        }

        internal void DispatchToUi(Action action)
        {
            if (_syncContext != null)
            {
                _syncContext.Post(_ => action(), null);
            }
            else
            {
                action();
            }
        }

        private void HandleNativeRegState(IntPtr client, int state, int code, string reason, IntPtr userData)
        {
            var regState = (SipRegistrationState)state;
            DispatchToUi(() =>
            {
                RegistrationStateChanged?.Invoke(this, new RegistrationStateChangedEventArgs(regState, code, reason));
            });
        }

        private void HandleNativeIncomingCall(IntPtr client, IntPtr callHandle, IntPtr userData)
        {
            var call = new SipCall(callHandle, this);
            _activeCalls[callHandle] = call;

            DispatchToUi(() =>
            {
                IncomingCall?.Invoke(this, new IncomingCallEventArgs(call));
            });
        }

        private void HandleNativeCallState(IntPtr callHandle, int state, int code, string reason, IntPtr userData)
        {
            if (_activeCalls.TryGetValue(callHandle, out var call))
            {
                call.HandleStateChange((SipCallState)state, code, reason);
            }
        }

        private void HandleNativeCallMedia(IntPtr callHandle, int width, int height, bool onHold, bool isMuted, IntPtr userData)
        {
            if (_activeCalls.TryGetValue(callHandle, out var call))
            {
                call.HandleMediaChange(width, height, onHold, isMuted);
            }
        }

        private void HandleNativeDtmfDigit(IntPtr callHandle, string digit, IntPtr userData)
        {
            if (_activeCalls.TryGetValue(callHandle, out var call))
            {
                call.HandleDtmf(digit);
            }
        }

        private void HandleNativeCallStats(IntPtr callHandle, IntPtr statsPtr, IntPtr userData)
        {
            if (statsPtr == IntPtr.Zero) return;
            if (_activeCalls.TryGetValue(callHandle, out var call))
            {
                var raw = Marshal.PtrToStructure<SipCallbacksStatsMarshal>(statsPtr);
                var stats = new SipCallStats(
                    raw.AudioCodec ?? string.Empty,
                    raw.AudioBitrateKbps,
                    raw.AudioLossRate,
                    raw.AudioJitterMs,
                    raw.AudioRttMs,
                    raw.VideoCodec ?? string.Empty,
                    raw.VideoBitrateKbps,
                    raw.VideoLossRate,
                    raw.VideoJitterMs,
                    raw.VideoFps);
                call.HandleStatsUpdate(stats);
            }
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
        private struct SipCallbacksStatsMarshal
        {
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
            public string AudioCodec;
            public uint AudioBitrateKbps;
            public float AudioLossRate;
            public uint AudioJitterMs;
            public uint AudioRttMs;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
            public string VideoCodec;
            public uint VideoBitrateKbps;
            public float VideoLossRate;
            public uint VideoJitterMs;
            public uint VideoFps;
        }

        private void HandleNativeTransferStatus(IntPtr callHandle, int code, string reason, IntPtr userData)
        {
            if (_activeCalls.TryGetValue(callHandle, out var call))
            {
                call.HandleTransferStatus(code, reason);
            }
        }

        private void HandleNativeTransferRequest(IntPtr callHandle, string referToUri, bool hasReplaces, IntPtr userData)
        {
            if (_activeCalls.TryGetValue(callHandle, out var call))
            {
                call.HandleTransferRequest(referToUri, hasReplaces);
            }
        }

        private void HandleNativeCallRejectedBusy(IntPtr client, string callerUri, IntPtr userData)
        {
            DispatchToUi(() =>
            {
                CallRejectedBusy?.Invoke(this, new CallRejectedBusyEventArgs(callerUri ?? string.Empty));
            });
        }

        private void HandleNativeConferenceChanged(IntPtr client, IntPtr callsPtr, UIntPtr count, IntPtr userData)
        {
            var members = new System.Collections.Generic.List<SipCall>();
            int n = (int)count;
            if (callsPtr != IntPtr.Zero && n > 0)
            {
                for (int i = 0; i < n; i++)
                {
                    IntPtr h = Marshal.ReadIntPtr(callsPtr, i * IntPtr.Size);
                    if (h != IntPtr.Zero && _activeCalls.TryGetValue(h, out var call))
                    {
                        members.Add(call);
                    }
                }
            }
            DispatchToUi(() =>
            {
                ConferenceChanged?.Invoke(this, new ConferenceChangedEventArgs(members));
            });
        }

        private void HandleNativeMessageReceived(IntPtr client, IntPtr fromUri, IntPtr contentType, IntPtr body, UIntPtr bodyLen, IntPtr userData)
        {
            string from = SipNativeAbi.PtrToStringUtf8(fromUri) ?? string.Empty;
            string ctype = SipNativeAbi.PtrToStringUtf8(contentType) ?? "text/plain";
            byte[] bytes = new byte[(int)bodyLen];
            if (body != IntPtr.Zero && bytes.Length > 0)
            {
                Marshal.Copy(body, bytes, 0, bytes.Length);
            }
            var args = new MessageReceivedEventArgs(from, ctype, bytes);
            DispatchToUi(() =>
            {
                MessageReceived?.Invoke(this, args);
            });
        }

        private void HandleNativeMessageStatus(IntPtr client, ulong messageId, int status, int code, IntPtr reason, IntPtr userData)
        {
            var args = new MessageStatusChangedEventArgs(messageId, (SipMessageStatus)status, code, SipNativeAbi.PtrToStringUtf8(reason) ?? string.Empty);
            DispatchToUi(() =>
            {
                MessageStatusChanged?.Invoke(this, args);
            });
        }

        private void HandleNativeRecordingState(IntPtr client, IntPtr callHandle, int state, IntPtr path, int errorCode, IntPtr userData)
        {
            if (!_activeCalls.TryGetValue(callHandle, out var call)) return;
            var args = new RecordingStateChangedEventArgs(call, (SipRecordingState)state, SipNativeAbi.PtrToStringUtf8(path) ?? string.Empty, errorCode);
            DispatchToUi(() =>
            {
                RecordingStateChanged?.Invoke(this, args);
            });
        }

        private void HandleNativeAccountRegState(IntPtr client, IntPtr accountHandle, int state, int code, IntPtr reason, IntPtr userData)
        {
            var account = ResolveAccount(accountHandle);
            var args = new SipAccountRegistrationStateEventArgs(
                account,
                (SipRegistrationState)state,
                code,
                SipNativeAbi.PtrToStringUtf8(reason) ?? string.Empty);
            DispatchToUi(() =>
            {
                AccountRegistrationStateChanged?.Invoke(this, args);
            });
        }

        private void HandleNativeAccountMessageReceived(IntPtr client, IntPtr accountHandle, IntPtr fromUri, IntPtr contentType, IntPtr body, UIntPtr bodyLen, IntPtr userData)
        {
            var account = ResolveAccount(accountHandle);
            string from = SipNativeAbi.PtrToStringUtf8(fromUri) ?? string.Empty;
            string ctype = SipNativeAbi.PtrToStringUtf8(contentType) ?? "text/plain";
            byte[] bytes = new byte[(int)bodyLen];
            if (body != IntPtr.Zero && bytes.Length > 0)
            {
                Marshal.Copy(body, bytes, 0, bytes.Length);
            }
            var args = new SipAccountMessageReceivedEventArgs(account, from, ctype, bytes);
            DispatchToUi(() =>
            {
                AccountMessageReceived?.Invoke(this, args);
            });
        }

        private void ThrowIfDisposed()
        {
            if (_disposed || _clientHandle == IntPtr.Zero)
            {
                throw new ObjectDisposedException(nameof(SipClient));
            }
        }

        public void Dispose()
        {
            if (!_disposed)
            {
                _disposed = true;
                foreach (var kvp in _activeCalls)
                {
                    kvp.Value.Dispose();
                }
                _activeCalls.Clear();

                foreach (var kvp in _accounts)
                {
                    kvp.Value.Dispose();
                }
                _accounts.Clear();

                Stop();

                if (_gcHandle.IsAllocated)
                {
                    _gcHandle.Free();
                }
            }
        }
    }
}

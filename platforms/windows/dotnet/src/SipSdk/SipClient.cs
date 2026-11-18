using System;
using System.Collections.Concurrent;
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
        private readonly NativeMessageReceivedCallback _messageReceivedCallback;
        private readonly NativeMessageStatusCallback _messageStatusCallback;

        private GCHandle _gcHandle;
        private bool _disposed;
        private string _accountAor = string.Empty;

        public int MaxCalls { get; set; } = 4;
        public bool IsRegistered => _clientHandle != IntPtr.Zero && SipNativeAbi.sip_client_is_registered(_clientHandle);
        public bool IsMicMuted => _clientHandle != IntPtr.Zero && SipNativeAbi.sip_client_is_mic_muted(_clientHandle);
        public string AccountAor => _accountAor;

        public event EventHandler<RegistrationStateChangedEventArgs>? RegistrationStateChanged;
        public event EventHandler<IncomingCallEventArgs>? IncomingCall;
        public event EventHandler<CallRejectedBusyEventArgs>? CallRejectedBusy;
        public event EventHandler<MessageReceivedEventArgs>? MessageReceived;
        public event EventHandler<MessageStatusChangedEventArgs>? MessageStatusChanged;

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
            _messageReceivedCallback = HandleNativeMessageReceived;
            _messageStatusCallback = HandleNativeMessageStatus;

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
                OnCallRejectedBusy = Marshal.GetFunctionPointerForDelegate(_callRejectedBusyCallback)
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

        internal void RemoveCall(IntPtr callHandle)
        {
            _activeCalls.TryRemove(callHandle, out _);
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

                Stop();

                if (_gcHandle.IsAllocated)
                {
                    _gcHandle.Free();
                }
            }
        }
    }
}

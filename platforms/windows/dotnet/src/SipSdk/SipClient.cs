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

        private GCHandle _gcHandle;
        private bool _disposed;
        private string _accountAor = string.Empty;

        public bool IsRegistered => _clientHandle != IntPtr.Zero && SipNativeAbi.sip_client_is_registered(_clientHandle);
        public bool IsMicMuted => _clientHandle != IntPtr.Zero && SipNativeAbi.sip_client_is_mic_muted(_clientHandle);
        public string AccountAor => _accountAor;

        public event EventHandler<RegistrationStateChangedEventArgs>? RegistrationStateChanged;
        public event EventHandler<IncomingCallEventArgs>? IncomingCall;

        public SipClient()
        {
            _syncContext = SynchronizationContext.Current;

            _regCallback = HandleNativeRegState;
            _incomingCallCallback = HandleNativeIncomingCall;
            _callStateCallback = HandleNativeCallState;
            _callMediaCallback = HandleNativeCallMedia;
            _dtmfCallback = HandleNativeDtmfDigit;

            _gcHandle = GCHandle.Alloc(this);
            IntPtr userData = GCHandle.ToIntPtr(_gcHandle);

            var callbacks = new SipCallbacksNative
            {
                OnRegState = Marshal.GetFunctionPointerForDelegate(_regCallback),
                OnIncomingCall = Marshal.GetFunctionPointerForDelegate(_incomingCallCallback),
                OnCallState = Marshal.GetFunctionPointerForDelegate(_callStateCallback),
                OnCallMedia = Marshal.GetFunctionPointerForDelegate(_callMediaCallback),
                OnDtmfDigit = Marshal.GetFunctionPointerForDelegate(_dtmfCallback)
            };

            _clientHandle = SipNativeAbi.sip_client_create(ref callbacks, userData);
            if (_clientHandle == IntPtr.Zero)
            {
                throw new InvalidOperationException("Failed to allocate native SIP client handle.");
            }
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

        public int Register(string username, string domain, string password, string transport = "udp", int port = 5060)
        {
            ThrowIfDisposed();
            if (string.IsNullOrEmpty(username)) throw new ArgumentException("Username cannot be empty", nameof(username));
            if (string.IsNullOrEmpty(domain)) throw new ArgumentException("Domain cannot be empty", nameof(domain));

            _accountAor = $"sip:{username}@{domain}:{port}";
            return SipNativeAbi.sip_client_register(_clientHandle, username, domain, password, transport, port);
        }

        public int Unregister()
        {
            ThrowIfDisposed();
            return SipNativeAbi.sip_client_unregister(_clientHandle);
        }

        public SipCall MakeCall(string targetUri, bool withVideo = false)
        {
            ThrowIfDisposed();
            if (string.IsNullOrEmpty(targetUri)) throw new ArgumentException("Target URI cannot be empty", nameof(targetUri));

            IntPtr callHandle = SipNativeAbi.sip_call_make(_clientHandle, targetUri, withVideo);
            if (callHandle == IntPtr.Zero)
            {
                throw new InvalidOperationException($"Failed to initiate call to {targetUri}");
            }

            var call = new SipCall(callHandle, this);
            _activeCalls[callHandle] = call;
            return call;
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

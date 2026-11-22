using System;
using System.Runtime.InteropServices;
using SipSdk.Native;

namespace SipSdk
{
    /// <summary>
    /// Represents an active or incoming SIP call session.
    /// </summary>
    public sealed class SipCall : IDisposable
    {
        private IntPtr _callHandle;
        private readonly SipClient _client;
        private SipCallState _state = SipCallState.Connecting;
        private bool _disposed;

        public IntPtr Handle => _callHandle;

        public string Id
        {
            get
            {
                if (_callHandle == IntPtr.Zero) return string.Empty;
                IntPtr ptr = SipNativeAbi.sip_call_get_id(_callHandle);
                return MarshalUtf8String(ptr);
            }
        }

        public string RemoteUri
        {
            get
            {
                if (_callHandle == IntPtr.Zero) return string.Empty;
                IntPtr ptr = SipNativeAbi.sip_call_get_remote_uri(_callHandle);
                return MarshalUtf8String(ptr);
            }
        }

        public string PeerDisplayName
        {
            get
            {
                if (_callHandle == IntPtr.Zero) return string.Empty;
                IntPtr ptr = SipNativeAbi.sip_call_get_peer_display_name(_callHandle);
                return MarshalUtf8String(ptr);
            }
        }

        public SipCallState State => _state;
        public bool HasVideo => _callHandle != IntPtr.Zero && SipNativeAbi.sip_call_has_video(_callHandle);
        public bool IsOutgoing => _callHandle != IntPtr.Zero && SipNativeAbi.sip_call_is_outgoing(_callHandle);
        public bool IsOnHold => _callHandle != IntPtr.Zero && SipNativeAbi.sip_call_is_on_hold(_callHandle);
        public bool IsMuted => _callHandle != IntPtr.Zero && SipNativeAbi.sip_call_is_muted(_callHandle);
        public bool IsClosed => _callHandle == IntPtr.Zero || SipNativeAbi.sip_call_is_closed(_callHandle);
        public bool IsSecure => _callHandle != IntPtr.Zero && SipNativeAbi.sip_call_is_secure(_callHandle);
        public bool IsInConference => _callHandle != IntPtr.Zero && SipNativeAbi.sip_call_is_in_conference(_callHandle);
        internal IntPtr NativeHandle => _callHandle;
        public uint Duration => _callHandle != IntPtr.Zero ? SipNativeAbi.sip_call_get_duration(_callHandle) : 0;

        public SipAccount? Account
        {
            get
            {
                if (_callHandle == IntPtr.Zero) return null;
                IntPtr accHandle = SipNativeAbi.sip_call_get_account(_callHandle);
                return _client.ResolveAccount(accHandle);
            }
        }

        public event EventHandler<CallStateChangedEventArgs>? StateChanged;
        public event EventHandler<CallMediaChangedEventArgs>? MediaChanged;
        public event EventHandler<DtmfReceivedEventArgs>? DtmfReceived;
        public event EventHandler<CallStatsEventArgs>? StatsUpdated;
        public event EventHandler<TransferStatusEventArgs>? TransferStatusChanged;
        public event EventHandler<TransferRequestEventArgs>? TransferRequested;

        internal SipCall(IntPtr callHandle, SipClient client)
        {
            _callHandle = callHandle;
            _client = client ?? throw new ArgumentNullException(nameof(client));
        }

        public int Answer(bool withVideo = false)
        {
            ThrowIfDisposed();
            return SipNativeAbi.sip_call_answer(_callHandle, withVideo);
        }

        public int Hangup(int statusCode = 0, string? reason = null)
        {
            ThrowIfDisposed();
            return SipNativeAbi.sip_call_hangup(_callHandle, statusCode, reason);
        }

        public int SetHold(bool hold)
        {
            ThrowIfDisposed();
            return SipNativeAbi.sip_call_set_hold(_callHandle, hold);
        }

        public int SetMuted(bool muted)
        {
            ThrowIfDisposed();
            return SipNativeAbi.sip_call_set_muted(_callHandle, muted);
        }

        public int SendDtmf(string digits)
        {
            ThrowIfDisposed();
            if (string.IsNullOrEmpty(digits)) throw new ArgumentException("Digits cannot be empty", nameof(digits));
            return SipNativeAbi.sip_call_send_dtmf(_callHandle, digits);
        }

        public int RequestKeyframe()
        {
            ThrowIfDisposed();
            return SipNativeAbi.sip_call_request_keyframe(_callHandle);
        }

        public SipCallStats GetStats()
        {
            ThrowIfDisposed();
            if (SipNativeAbi.sip_call_get_stats(_callHandle, out var raw) == 0)
            {
                return new SipCallStats(
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
            }
            return new SipCallStats(string.Empty, 0, 0f, 0, 0, string.Empty, 0, 0f, 0, 0);
        }

        public int Transfer(string targetUri)
        {
            ThrowIfDisposed();
            if (string.IsNullOrEmpty(targetUri)) throw new ArgumentException("Target URI cannot be empty", nameof(targetUri));
            return SipNativeAbi.sip_call_transfer(_callHandle, targetUri);
        }

        public int AttendedTransfer(SipCall consultCall)
        {
            ThrowIfDisposed();
            if (consultCall == null) throw new ArgumentNullException(nameof(consultCall));
            consultCall.ThrowIfDisposed();
            return SipNativeAbi.sip_call_attended_transfer(_callHandle, consultCall._callHandle);
        }

        public int AcceptTransfer(bool accept)
        {
            ThrowIfDisposed();
            return SipNativeAbi.sip_call_accept_transfer(_callHandle, accept);
        }

        internal void HandleStatsUpdate(SipCallStats stats)
        {
            _client.DispatchToUi(() =>
            {
                StatsUpdated?.Invoke(this, new CallStatsEventArgs(stats));
            });
        }

        internal void HandleTransferStatus(int code, string reason)
        {
            _client.DispatchToUi(() =>
            {
                TransferStatusChanged?.Invoke(this, new TransferStatusEventArgs(code, reason));
            });
        }

        internal void HandleTransferRequest(string referToUri, bool hasReplaces)
        {
            _client.DispatchToUi(() =>
            {
                TransferRequested?.Invoke(this, new TransferRequestEventArgs(referToUri, hasReplaces));
            });
        }

        internal void HandleStateChange(SipCallState state, int code, string reason)
        {
            _state = state;
            _client.DispatchToUi(() =>
            {
                StateChanged?.Invoke(this, new CallStateChangedEventArgs(state, code, reason));
            });

            if (state == SipCallState.Closed)
            {
                _client.RemoveCall(_callHandle);
            }
        }

        internal void HandleMediaChange(int width, int height, bool onHold, bool isMuted)
        {
            _client.DispatchToUi(() =>
            {
                MediaChanged?.Invoke(this, new CallMediaChangedEventArgs(width, height, onHold, isMuted));
            });
        }

        internal void HandleDtmf(string digit)
        {
            _client.DispatchToUi(() =>
            {
                DtmfReceived?.Invoke(this, new DtmfReceivedEventArgs(digit));
            });
        }

        private static string MarshalUtf8String(IntPtr ptr)
        {
            if (ptr == IntPtr.Zero) return string.Empty;
#if NET5_0_OR_GREATER || NETCOREAPP1_1_OR_GREATER
            return Marshal.PtrToStringUTF8(ptr) ?? string.Empty;
#else
            int len = 0;
            while (Marshal.ReadByte(ptr, len) != 0) len++;
            byte[] bytes = new byte[len];
            Marshal.Copy(ptr, bytes, 0, len);
            return System.Text.Encoding.UTF8.GetString(bytes);
#endif
        }

        private void ThrowIfDisposed()
        {
            if (_disposed || _callHandle == IntPtr.Zero)
            {
                throw new ObjectDisposedException(nameof(SipCall));
            }
        }

        public void Dispose()
        {
            if (!_disposed)
            {
                _disposed = true;
                if (_callHandle != IntPtr.Zero)
                {
                    _client.RemoveCall(_callHandle);
                    SipNativeAbi.sip_call_destroy(_callHandle);
                    _callHandle = IntPtr.Zero;
                }
            }
        }
    }
}

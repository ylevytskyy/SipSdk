using System;

namespace SipSdk
{
    /// <summary>
    /// Event arguments for changes in SIP registration state.
    /// </summary>
    public class RegistrationStateChangedEventArgs : EventArgs
    {
        public SipRegistrationState State { get; }
        public int StatusCode { get; }
        public string Reason { get; }

        public RegistrationStateChangedEventArgs(SipRegistrationState state, int statusCode, string reason)
        {
            State = state;
            StatusCode = statusCode;
            Reason = reason ?? string.Empty;
        }
    }

    /// <summary>
    /// Event arguments for incoming call notifications.
    /// </summary>
    public class IncomingCallEventArgs : EventArgs
    {
        public SipCall Call { get; }

        public IncomingCallEventArgs(SipCall call)
        {
            Call = call ?? throw new ArgumentNullException(nameof(call));
        }
    }

    /// <summary>
    /// Event arguments for call session state changes.
    /// </summary>
    public class CallStateChangedEventArgs : EventArgs
    {
        public SipCallState State { get; }
        public int StatusCode { get; }
        public string Reason { get; }

        public CallStateChangedEventArgs(SipCallState state, int statusCode, string reason)
        {
            State = state;
            StatusCode = statusCode;
            Reason = reason ?? string.Empty;
        }
    }

    /// <summary>
    /// Event arguments for call media stream updates (video resolution, hold, mute).
    /// </summary>
    public class CallMediaChangedEventArgs : EventArgs
    {
        public int Width { get; }
        public int Height { get; }
        public bool IsOnHold { get; }
        public bool IsMuted { get; }

        public CallMediaChangedEventArgs(int width, int height, bool isOnHold, bool isMuted)
        {
            Width = width;
            Height = height;
            IsOnHold = isOnHold;
            IsMuted = isMuted;
        }
    }

    /// <summary>
    /// Event arguments for received RFC 4733 DTMF digits.
    /// </summary>
    public class DtmfReceivedEventArgs : EventArgs
    {
        public string Digit { get; }

        public DtmfReceivedEventArgs(string digit)
        {
            Digit = digit ?? string.Empty;
        }
    }

    /// <summary>
    /// Event arguments for a received SIP MESSAGE.
    /// </summary>
    public class MessageReceivedEventArgs : EventArgs
    {
        /// <summary>Sender URI, e.g. "sip:1002@pbx".</summary>
        public string From { get; }
        /// <summary>MIME type, e.g. "text/plain".</summary>
        public string ContentType { get; }
        /// <summary>Raw body bytes.</summary>
        public byte[] Body { get; }

        /// <summary>Body as UTF-8 text for text/* content types; null otherwise.</summary>
        public string? Text => ContentType.StartsWith("text/", StringComparison.OrdinalIgnoreCase)
            ? System.Text.Encoding.UTF8.GetString(Body)
            : null;

        public MessageReceivedEventArgs(string from, string contentType, byte[] body)
        {
            From = from ?? string.Empty;
            ContentType = contentType ?? string.Empty;
            Body = body ?? Array.Empty<byte>();
        }
    }

    /// <summary>
    /// Event arguments for a status update of a sent SIP MESSAGE.
    /// </summary>
    public class MessageStatusChangedEventArgs : EventArgs
    {
        public ulong MessageId { get; }
        public SipMessageStatus Status { get; }
        /// <summary>SIP status code; 0 when no response was received.</summary>
        public int StatusCode { get; }
        public string Reason { get; }

        public MessageStatusChangedEventArgs(ulong messageId, SipMessageStatus status, int statusCode, string reason)
        {
            MessageId = messageId;
            Status = status;
            StatusCode = statusCode;
            Reason = reason ?? string.Empty;
        }
    }

    /// <summary>
    /// Real-time audio and video quality telemetry metrics for an active call.
    /// </summary>
    public sealed class SipCallStats
    {
        public string AudioCodec { get; }
        public uint AudioBitrateKbps { get; }
        public float AudioLossRate { get; }
        public uint AudioJitterMs { get; }
        public uint AudioRttMs { get; }
        public string VideoCodec { get; }
        public uint VideoBitrateKbps { get; }
        public float VideoLossRate { get; }
        public uint VideoJitterMs { get; }
        public uint VideoFps { get; }

        public SipCallStats(
            string audioCodec,
            uint audioBitrateKbps,
            float audioLossRate,
            uint audioJitterMs,
            uint audioRttMs,
            string videoCodec,
            uint videoBitrateKbps,
            float videoLossRate,
            uint videoJitterMs,
            uint videoFps)
        {
            AudioCodec = audioCodec ?? string.Empty;
            AudioBitrateKbps = audioBitrateKbps;
            AudioLossRate = audioLossRate;
            AudioJitterMs = audioJitterMs;
            AudioRttMs = audioRttMs;
            VideoCodec = videoCodec ?? string.Empty;
            VideoBitrateKbps = videoBitrateKbps;
            VideoLossRate = videoLossRate;
            VideoJitterMs = videoJitterMs;
            VideoFps = videoFps;
        }
    }

    /// <summary>
    /// Event arguments for periodic real-time call quality telemetry updates.
    /// </summary>
    public class CallStatsEventArgs : EventArgs
    {
        public SipCallStats Stats { get; }

        public CallStatsEventArgs(SipCallStats stats)
        {
            Stats = stats ?? throw new ArgumentNullException(nameof(stats));
        }
    }

    /// <summary>
    /// Event arguments for outgoing call transfer (REFER) status progression.
    /// </summary>
    public class TransferStatusEventArgs : EventArgs
    {
        public int StatusCode { get; }
        public string Reason { get; }

        public TransferStatusEventArgs(int statusCode, string reason)
        {
            StatusCode = statusCode;
            Reason = reason ?? string.Empty;
        }
    }

    /// <summary>
    /// Event arguments for incoming call transfer requests (REFER) from the remote peer.
    /// </summary>
    public class TransferRequestEventArgs : EventArgs
    {
        public string ReferToUri { get; }
        public bool HasReplaces { get; }

        public TransferRequestEventArgs(string referToUri, bool hasReplaces)
        {
            ReferToUri = referToUri ?? string.Empty;
            HasReplaces = hasReplaces;
        }
    }

    public class CallRejectedBusyEventArgs : EventArgs
    {
        public string CallerUri { get; }

        public CallRejectedBusyEventArgs(string callerUri)
        {
            CallerUri = callerUri ?? string.Empty;
        }
    }

    public class SipBusyException : InvalidOperationException
    {
        public SipBusyException(string message) : base(message) { }
    }

    public class ConferenceChangedEventArgs : EventArgs
    {
        public System.Collections.Generic.IReadOnlyList<SipCall> Calls { get; }

        public ConferenceChangedEventArgs(System.Collections.Generic.IReadOnlyList<SipCall> calls)
        {
            Calls = calls ?? Array.Empty<SipCall>();
        }
    }
}

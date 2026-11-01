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
}

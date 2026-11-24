namespace SipSdk
{
    /// <summary>
    /// Registration status of the SIP user agent with the registrar server.
    /// </summary>
    public enum SipRegistrationState
    {
        Unregistered = 0,
        Registering = 1,
        Registered = 2,
        Failed = 3
    }

    /// <summary>
    /// Telephony call session state progression.
    /// </summary>
    public enum SipCallState
    {
        Incoming = 0,
        Ringing = 1,
        Connecting = 2,
        Established = 3,
        Hold = 4,
        Closed = 5
    }

    /// <summary>
    /// Status of a message sent with <see cref="SipClient.SendMessage(string, string)"/>.
    /// </summary>
    public enum SipMessageStatus
    {
        /// <summary>The server or peer accepted the message (2xx).</summary>
        Sent = 0,
        /// <summary>Non-2xx final response, transport error, or no registered account.</summary>
        Failed = 1,
        /// <summary>Reserved: delivery notification (IMDN, RFC 5438) from the peer.</summary>
        Delivered = 2,
        /// <summary>Reserved: display (read) notification (IMDN, RFC 5438) from the peer.</summary>
        Displayed = 3
    }

    public enum SipRecordingMode
    {
        AudioStereo = 0,
        AudioMono = 1,
        AudioVideo = 2
    }

    public enum SipRecordingState
    {
        Idle = 0,
        Recording = 1,
        Failed = 2
    }
}

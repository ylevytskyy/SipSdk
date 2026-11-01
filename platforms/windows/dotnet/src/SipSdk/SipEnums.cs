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
}

using System;
using Xunit;

namespace SipSdk.Tests
{
    public class SipCallTests
    {
        [Fact]
        public void CallSession_EventArgs_PropertiesAssignedCorrectly()
        {
            var regArgs = new RegistrationStateChangedEventArgs(SipRegistrationState.Registered, 200, "OK");
            Assert.Equal(SipRegistrationState.Registered, regArgs.State);
            Assert.Equal(200, regArgs.StatusCode);
            Assert.Equal("OK", regArgs.Reason);

            var callStateArgs = new CallStateChangedEventArgs(SipCallState.Established, 200, "Answered");
            Assert.Equal(SipCallState.Established, callStateArgs.State);
            Assert.Equal(200, callStateArgs.StatusCode);
            Assert.Equal("Answered", callStateArgs.Reason);

            var mediaArgs = new CallMediaChangedEventArgs(1280, 720, false, false);
            Assert.Equal(1280, mediaArgs.Width);
            Assert.Equal(720, mediaArgs.Height);
            Assert.False(mediaArgs.IsOnHold);
            Assert.False(mediaArgs.IsMuted);

            var dtmfArgs = new DtmfReceivedEventArgs("5");
            Assert.Equal("5", dtmfArgs.Digit);
        }

        [Fact]
        public void CallSession_EnumValues_MatchExpectedConstants()
        {
            Assert.Equal(0, (int)SipCallState.Incoming);
            Assert.Equal(1, (int)SipCallState.Ringing);
            Assert.Equal(2, (int)SipCallState.Connecting);
            Assert.Equal(3, (int)SipCallState.Established);
            Assert.Equal(4, (int)SipCallState.Hold);
            Assert.Equal(5, (int)SipCallState.Closed);

            Assert.Equal(0, (int)SipRegistrationState.Unregistered);
            Assert.Equal(1, (int)SipRegistrationState.Registering);
            Assert.Equal(2, (int)SipRegistrationState.Registered);
            Assert.Equal(3, (int)SipRegistrationState.Failed);
        }
    }
}

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

            var stats = new SipCallStats("opus", 64, 0.5f, 4, 15, "H264", 512, 0.1f, 8, 30);
            Assert.Equal("opus", stats.AudioCodec);
            Assert.Equal(64u, stats.AudioBitrateKbps);
            Assert.Equal(0.5f, stats.AudioLossRate);
            Assert.Equal(4u, stats.AudioJitterMs);
            Assert.Equal(15u, stats.AudioRttMs);
            Assert.Equal("H264", stats.VideoCodec);
            Assert.Equal(512u, stats.VideoBitrateKbps);
            Assert.Equal(0.1f, stats.VideoLossRate);
            Assert.Equal(8u, stats.VideoJitterMs);
            Assert.Equal(30u, stats.VideoFps);

            var statsArgs = new CallStatsEventArgs(stats);
            Assert.Same(stats, statsArgs.Stats);

            var transferStatusArgs = new TransferStatusEventArgs(200, "Call transferred");
            Assert.Equal(200, transferStatusArgs.StatusCode);
            Assert.Equal("Call transferred", transferStatusArgs.Reason);

            var transferReqArgs = new TransferRequestEventArgs("sip:9002@127.0.0.1", true);
            Assert.Equal("sip:9002@127.0.0.1", transferReqArgs.ReferToUri);
            Assert.True(transferReqArgs.HasReplaces);
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

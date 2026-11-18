using System;
using System.IO;
using System.Threading;
using Xunit;

namespace SipSdk.Tests
{
    [Collection("SipClientSequential")]
    public class SipClientTests
    {
        [Fact]
        public void Client_CreationAndLifecycle_DisposesCleanly()
        {
            using var client = new SipClient();
            Assert.NotNull(client);
            Assert.False(client.IsRegistered);
            Assert.False(client.IsMicMuted);
            Assert.False(client.IsLocalPreviewActive());
        }

        [Fact]
        public void Client_MicrophoneMute_Toggles()
        {
            using var client = new SipClient();
            client.SetMicMuted(true);
            Assert.True(client.IsMicMuted);

            client.SetMicMuted(false);
            Assert.False(client.IsMicMuted);
        }

        [Fact]
        public void Client_CameraControls_ExecuteWithoutThrowing()
        {
            using var client = new SipClient();
            client.SetCamera(1);
            client.SwitchCamera();
            client.SetLocalVideoEnabled(true);
            Assert.True(client.IsLocalVideoEnabled());
        }

        [Fact]
        public void Client_RegistrationStateChanged_EventSubscription()
        {
            using var client = new SipClient();
            bool eventFired = false;
            client.RegistrationStateChanged += (sender, args) =>
            {
                eventFired = true;
            };

            Assert.False(eventFired);
        }

        [Fact]
        public void Client_SynchronizationContext_MarshalsToTargetContext()
        {
            var testContext = new TrackingSyncContext();
            var prevContext = SynchronizationContext.Current;
            try
            {
                SynchronizationContext.SetSynchronizationContext(testContext);
                using var client = new SipClient();

                client.DispatchToUi(() =>
                {
                    // Action dispatched via context
                });

                Assert.True(testContext.PostCalled);
            }
            finally
            {
                SynchronizationContext.SetSynchronizationContext(prevContext);
            }
        }

        [Fact]
        public void Client_SendMessage_RejectsInvalidArguments()
        {
            using var client = new SipClient();
            Assert.Throws<ArgumentException>(() => client.SendMessage("", "hello"));
            Assert.Throws<ArgumentException>(() => client.SendMessage("sip:1001@127.0.0.1", ""));
            Assert.Throws<ArgumentException>(() => client.SendMessage("sip:1001@127.0.0.1", new byte[] { 1 }, ""));
        }

        [Fact]
        public void Client_SendMessage_BeforeStart_Throws()
        {
            using var client = new SipClient();
            Assert.Throws<InvalidOperationException>(() => client.SendMessage("sip:1001@127.0.0.1", "hello"));
        }

        [Fact]
        public void Client_SendMessage_WithoutAccount_ReportsFailedStatus()
        {
            string configDir = Path.Combine(Path.GetTempPath(), "sipsdk-dotnet-msg-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(configDir);
            File.WriteAllText(Path.Combine(configDir, "config"), "sip_listen 127.0.0.1:0\n");
            try
            {
                using var client = new SipClient();
                using var statusReceived = new ManualResetEventSlim(false);
                MessageStatusChangedEventArgs? status = null;
                client.MessageStatusChanged += (sender, args) =>
                {
                    status = args;
                    statusReceived.Set();
                };

                Assert.Equal(0, client.Start(configDir));
                ulong id = client.SendMessage("sip:1001@127.0.0.1:5060", "hello from .NET");
                Assert.True(id > 0);

                Assert.True(statusReceived.Wait(TimeSpan.FromSeconds(5)), "No MessageStatusChanged event");
                Assert.NotNull(status);
                Assert.Equal(id, status!.MessageId);
                Assert.Equal(SipMessageStatus.Failed, status.Status);
                Assert.False(string.IsNullOrEmpty(status.Reason));
            }
            finally
            {
                Directory.Delete(configDir, true);
            }
        }

        [Fact]
        public void MessageReceivedEventArgs_DecodesUtf8Text()
        {
            byte[] body = System.Text.Encoding.UTF8.GetBytes("héllo");
            var text = new MessageReceivedEventArgs("sip:1002@pbx", "text/plain", body);
            Assert.Equal("héllo", text.Text);
            var json = new MessageReceivedEventArgs("sip:1002@pbx", "application/json", body);
            Assert.Null(json.Text);
            Assert.Equal(body, json.Body);
        }

        [Fact]
        public void Client_RegisterWithInvalidTlsCa_ReportsFailedRegistration()
        {
            string configDir = Path.Combine(Path.GetTempPath(), "sipsdk-dotnet-tls-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(configDir);
            File.WriteAllText(Path.Combine(configDir, "config"), "sip_listen 127.0.0.1:0\n");
            try
            {
                using var client = new SipClient();
                using var regFailed = new ManualResetEventSlim(false);
                RegistrationStateChangedEventArgs? lastReg = null;
                client.RegistrationStateChanged += (sender, args) =>
                {
                    if (args.State == SipRegistrationState.Failed)
                    {
                        lastReg = args;
                        regFailed.Set();
                    }
                };

                Assert.Equal(0, client.Start(configDir));
                int rc = client.Register(
                    "1004",
                    "127.0.0.1",
                    "test1004",
                    "tls",
                    5061,
                    tlsCaFile: "/nonexistent/ca.pem",
                    verifyTlsCert: true,
                    mediaEnc: "srtp-mand");
                Assert.Equal(0, rc);
                Assert.True(regFailed.Wait(TimeSpan.FromSeconds(5)), "Expected Failed registration state for missing CA file");
                Assert.NotNull(lastReg);
                Assert.Equal(SipRegistrationState.Failed, lastReg!.State);
            }
            finally
            {
                Directory.Delete(configDir, true);
            }
        }

        [Fact]
        public void Client_MultiCallMaxCallsAndBusyEvent_ConfiguredCleanly()
        {
            using var client = new SipClient();
            Assert.Equal(4, client.MaxCalls);
            client.MaxCalls = 1;
            Assert.Equal(1, client.MaxCalls);

            bool busySubscribed = false;
            client.CallRejectedBusy += (sender, args) =>
            {
                busySubscribed = !string.IsNullOrEmpty(args.CallerUri);
            };
            Assert.False(busySubscribed);

            // Validate ABI P/Invoke dial entrypoint rejects empty/null arguments cleanly.
            Assert.Throws<ArgumentException>(() => client.Dial("", false));

            string configDir = Path.Combine(Path.GetTempPath(), "sipsdk-dotnet-cap-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(configDir);
            File.WriteAllText(Path.Combine(configDir, "config"), "sip_listen 127.0.0.1:0\n");
            try
            {
                Assert.Equal(0, client.Start(configDir));
                client.Register("1001", "127.0.0.1", "test1001", "udp", 5060);
                var call1 = client.Dial("sip:9001@127.0.0.1:5060", false);
                Assert.NotNull(call1);

                // Second dial over MaxCalls=1 must throw InvalidOperationException / SipBusyException
                Assert.Throws<SipBusyException>(() => client.Dial("sip:9001@127.0.0.1:5060", false));
                call1.Hangup();
            }
            finally
            {
                Directory.Delete(configDir, true);
            }
        }

        private class TrackingSyncContext : SynchronizationContext
        {
            public bool PostCalled { get; private set; }

            public override void Post(SendOrPostCallback d, object? state)
            {
                PostCalled = true;
                d(state);
            }
        }
    }
}

using System;
using System.Threading;
using Xunit;

namespace SipSdk.Tests
{
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

using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using SipSdk.Native;
using Xunit;

namespace SipSdk.Tests
{
    [Collection("SipClientSequential")]
    public class SipAccountTests
    {
        private static string CreateTempConfigDir(string prefix)
        {
            string configDir = Path.Combine(Path.GetTempPath(), prefix + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(configDir);
            File.WriteAllText(Path.Combine(configDir, "config"), "sip_listen 127.0.0.1:0\n");
            return configDir;
        }

        [Fact]
        public void AccountCallbacks_StructLayoutAndEventArgs_AreValid()
        {
            Assert.Equal(IntPtr.Size * 2, Marshal.SizeOf<SipAccountCallbacks>());

            var regArgs = new SipAccountRegistrationStateEventArgs(null, SipRegistrationState.Registered, 200, "OK");
            Assert.Null(regArgs.Account);
            Assert.Equal(SipRegistrationState.Registered, regArgs.State);
            Assert.Equal(200, regArgs.Code);
            Assert.Equal(200, regArgs.StatusCode);
            Assert.Equal("OK", regArgs.Reason);

            byte[] utf8 = System.Text.Encoding.UTF8.GetBytes("héllo multi-account");
            var textMsg = new SipAccountMessageReceivedEventArgs(null, "sip:1001@127.0.0.1", "text/plain", utf8);
            Assert.Null(textMsg.Account);
            Assert.Equal("sip:1001@127.0.0.1", textMsg.FromUri);
            Assert.Equal("text/plain", textMsg.ContentType);
            Assert.Equal("héllo multi-account", textMsg.Text);

            var binMsg = new SipAccountMessageReceivedEventArgs(null, "sip:1001@127.0.0.1", "application/octet-stream", utf8);
            Assert.Null(binMsg.Text);
            Assert.Equal(utf8, binMsg.Body);
        }

        [Fact]
        public void Account_NullAndBeforeStartGuards_ThrowExpectedExceptions()
        {
            using var client = new SipClient();
            Assert.Empty(client.Accounts);
            Assert.Null(client.DefaultAccount);

            Assert.Throws<ArgumentNullException>(() => client.AddAccount(null!));
            Assert.Throws<ArgumentException>(() => client.AddAccount(new SipAccountConfig("", "127.0.0.1")));
            Assert.Throws<ArgumentException>(() => client.AddAccount(new SipAccountConfig("1001", "")));
            Assert.Throws<ArgumentNullException>(() => client.RemoveAccount(null!));
            Assert.Throws<ArgumentNullException>(() => client.SetDefaultAccount(null!));

            // Calling AddAccount before client.Start() returns EIO -> InvalidOperationException
            Assert.Throws<InvalidOperationException>(() => client.AddAccount(new SipAccountConfig("1001", "127.0.0.1", "pass")));
        }

        [Fact]
        public void Account_Lifecycle_AddListSwitchDefaultAndRemove()
        {
            string configDir = CreateTempConfigDir("sipsdk-dotnet-acc-life-");
            try
            {
                using var client = new SipClient();
                Assert.Equal(0, client.Start(configDir));

                using var acc1 = client.AddAccount(new SipAccountConfig("1001", "127.0.0.1", "test1001", "udp", 5060));
                Assert.True(acc1.Id > 0);
                Assert.Equal("1001", acc1.User);
                Assert.Equal("127.0.0.1", acc1.Domain);
                Assert.Contains("1001@127.0.0.1", acc1.Aor);
                Assert.Single(client.Accounts);
                Assert.Same(acc1, client.DefaultAccount);

                using var acc2 = client.AddAccount("1003", "127.0.0.1", "test1003", "udp", 5060);
                Assert.True(acc2.Id > 0);
                Assert.NotEqual(acc1.Id, acc2.Id);
                Assert.Equal(2, client.Accounts.Count);
                Assert.Same(acc1, client.DefaultAccount);

                client.SetDefaultAccount(acc2);
                Assert.Same(acc2, client.DefaultAccount);

                // Removing default account re-elects the remaining account
                client.RemoveAccount(acc2);
                Assert.Single(client.Accounts);
                Assert.Same(acc1, client.DefaultAccount);

                client.RemoveAccount(acc1);
                Assert.Empty(client.Accounts);
                Assert.Null(client.DefaultAccount);
            }
            finally
            {
                Directory.Delete(configDir, true);
            }
        }

        [Fact]
        public void Account_DuplicateAor_ThrowsInvalidOperationException()
        {
            string configDir = CreateTempConfigDir("sipsdk-dotnet-acc-dup-");
            try
            {
                using var client = new SipClient();
                Assert.Equal(0, client.Start(configDir));

                using var acc1 = client.AddAccount(new SipAccountConfig("1001", "127.0.0.1", "test1001", "udp", 5060));
                Assert.Throws<InvalidOperationException>(() =>
                    client.AddAccount(new SipAccountConfig("1001", "127.0.0.1", "test1001", "udp", 5060)));
            }
            finally
            {
                Directory.Delete(configDir, true);
            }
        }

        [Fact]
        public void Account_MaxCapEight_NinthThrowsSipBusyException()
        {
            string configDir = CreateTempConfigDir("sipsdk-dotnet-acc-cap-");
            try
            {
                using var client = new SipClient();
                Assert.Equal(0, client.Start(configDir));

                var added = new SipAccount[8];
                for (int i = 0; i < 8; i++)
                {
                    added[i] = client.AddAccount(new SipAccountConfig($"200{i}", "127.0.0.1", "pass", "udp", 5060));
                }
                Assert.Equal(8, client.Accounts.Count);

                Assert.Throws<SipBusyException>(() =>
                    client.AddAccount(new SipAccountConfig("2008", "127.0.0.1", "pass", "udp", 5060)));

                foreach (var acc in added)
                {
                    acc.Dispose();
                }
            }
            finally
            {
                Directory.Delete(configDir, true);
            }
        }

        [Fact]
        public void Account_Dial_CallAttributedToOwningAccount()
        {
            string configDir = CreateTempConfigDir("sipsdk-dotnet-acc-dial-");
            try
            {
                using var client = new SipClient();
                Assert.Equal(0, client.Start(configDir));

                using var acc1 = client.AddAccount(new SipAccountConfig("1001", "127.0.0.1", "test1001", "udp", 5060));
                using var acc2 = client.AddAccount(new SipAccountConfig("1003", "127.0.0.1", "test1003", "udp", 5060));

                Assert.Throws<ArgumentException>(() => acc2.Dial(""));

                using var call = acc2.Dial("sip:9001@127.0.0.1:5060", false);
                Assert.NotNull(call);
                Assert.NotNull(call.Account);
                Assert.Same(acc2, call.Account);
                Assert.Equal(acc2.Id, call.Account!.Id);
                call.Hangup();
            }
            finally
            {
                Directory.Delete(configDir, true);
            }
        }

        [Fact]
        public void Account_RegistrationStateChanged_FiresWithAccount()
        {
            string configDir = CreateTempConfigDir("sipsdk-dotnet-acc-ev-");
            try
            {
                using var client = new SipClient();
                using var failedEvent = new ManualResetEventSlim(false);
                SipAccountRegistrationStateEventArgs? received = null;

                client.AccountRegistrationStateChanged += (sender, args) =>
                {
                    if (args.State == SipRegistrationState.Failed)
                    {
                        received = args;
                        failedEvent.Set();
                    }
                };

                Assert.Equal(0, client.Start(configDir));
                using var acc = client.AddAccount(new SipAccountConfig
                {
                    Username = "1004",
                    Domain = "127.0.0.1",
                    Password = "test1004",
                    Transport = "tls",
                    Port = 5061,
                    TlsCaFile = "/nonexistent/ca.pem",
                    VerifyTlsCert = true,
                    MediaEnc = "srtp-mand"
                });

                Assert.True(failedEvent.Wait(TimeSpan.FromSeconds(5)), "Expected AccountRegistrationStateChanged Failed event");
                Assert.NotNull(received);
                Assert.Same(acc, received!.Account);
                Assert.Equal(SipRegistrationState.Failed, received.State);
            }
            finally
            {
                Directory.Delete(configDir, true);
            }
        }
    }
}

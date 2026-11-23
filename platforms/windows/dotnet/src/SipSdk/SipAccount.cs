using System;
using System.Threading;
using SipSdk.Native;

namespace SipSdk
{
    /// <summary>
    /// Configuration parameters for adding or registering a SIP account.
    /// </summary>
    public sealed class SipAccountConfig
    {
        public string Username { get; set; } = string.Empty;
        public string Domain { get; set; } = string.Empty;
        public string? Password { get; set; }
        public string Transport { get; set; } = "udp";
        public int Port { get; set; } = 5060;
        public string? DisplayName { get; set; }
        public string? OutboundProxy { get; set; }
        public string? StunServer { get; set; }
        public string? StunUser { get; set; }
        public string? StunPass { get; set; }
        public string? MediaNat { get; set; }
        public uint RegInterval { get; set; }
        public string? TlsCaFile { get; set; }
        public bool VerifyTlsCert { get; set; } = true;
        public string? MediaEnc { get; set; }
        public int MaxCalls { get; set; }

        public SipAccountConfig() { }

        public SipAccountConfig(
            string username,
            string domain,
            string? password = null,
            string transport = "udp",
            int port = 5060)
        {
            Username = username;
            Domain = domain;
            Password = password;
            Transport = transport;
            Port = port;
        }
    }

    /// <summary>
    /// Represents a SIP account registered on a <see cref="SipClient"/>.
    /// </summary>
    public sealed class SipAccount : IDisposable
    {
        private IntPtr _accountHandle;
        private readonly SipClient _client;
        private volatile bool _ownsHandle;
        private volatile bool _disposed;
        private readonly ulong _id;
        private readonly string _aor;
        private readonly string _user;
        private readonly string _domain;

        public IntPtr Handle => _accountHandle;
        internal IntPtr NativeHandle => _accountHandle;

        public ulong Id => _id;
        public string Aor => _aor;
        public string User => _user;
        public string Domain => _domain;

        public bool IsRegistered
        {
            get
            {
                IntPtr handle = Volatile.Read(ref _accountHandle);
                return handle != IntPtr.Zero && SipNativeAbi.sip_account_is_registered(handle);
            }
        }

        public SipRegistrationState RegistrationState
        {
            get
            {
                IntPtr handle = Volatile.Read(ref _accountHandle);
                return handle != IntPtr.Zero
                    ? (SipRegistrationState)SipNativeAbi.sip_account_get_reg_state(handle)
                    : SipRegistrationState.Unregistered;
            }
        }

        internal SipAccount(IntPtr accountHandle, SipClient client, bool ownsHandle = true)
        {
            _accountHandle = accountHandle;
            _client = client ?? throw new ArgumentNullException(nameof(client));
            _ownsHandle = ownsHandle;
            if (accountHandle != IntPtr.Zero)
            {
                _id = SipNativeAbi.sip_account_get_id(accountHandle);
                _aor = SipNativeAbi.PtrToStringUtf8(SipNativeAbi.sip_account_get_aor(accountHandle)) ?? string.Empty;
                _user = SipNativeAbi.PtrToStringUtf8(SipNativeAbi.sip_account_get_user(accountHandle)) ?? string.Empty;
                _domain = SipNativeAbi.PtrToStringUtf8(SipNativeAbi.sip_account_get_domain(accountHandle)) ?? string.Empty;
            }
            else
            {
                _aor = string.Empty;
                _user = string.Empty;
                _domain = string.Empty;
            }
        }

        internal void DetachHandle()
        {
            Interlocked.Exchange(ref _accountHandle, IntPtr.Zero);
        }

        internal void PromoteToOwned()
        {
            _ownsHandle = true;
        }

        public int Reregister()
        {
            ThrowIfDisposed();
            return SipNativeAbi.sip_account_reregister(_accountHandle);
        }

        public int Unregister()
        {
            ThrowIfDisposed();
            return SipNativeAbi.sip_account_unregister(_accountHandle);
        }

        public SipCall Dial(string targetUri, bool withVideo = false)
        {
            ThrowIfDisposed();
            if (string.IsNullOrEmpty(targetUri)) throw new ArgumentException("Target URI cannot be empty", nameof(targetUri));

            int rc = SipNativeAbi.sip_account_dial(_accountHandle, targetUri, withVideo, out IntPtr callHandle);
            if (rc == 16)
            {
                throw new SipBusyException($"Maximum concurrent calls reached ({_client.MaxCalls})");
            }
            if (rc != 0 || callHandle == IntPtr.Zero)
            {
                throw new InvalidOperationException($"Failed to initiate call to {targetUri} (error {rc})");
            }

            return _client.RegisterCall(callHandle);
        }

        public ulong SendMessage(string peerUri, string text)
        {
            if (string.IsNullOrEmpty(text)) throw new ArgumentException("Message text cannot be empty", nameof(text));
            return SendMessage(peerUri, System.Text.Encoding.UTF8.GetBytes(text), "text/plain");
        }

        public ulong SendMessage(string peerUri, string contentType, byte[] body)
        {
            return SendMessage(peerUri, body, contentType);
        }

        public ulong SendMessage(string peerUri, byte[] body, string contentType)
        {
            ThrowIfDisposed();
            if (string.IsNullOrEmpty(peerUri)) throw new ArgumentException("Target URI cannot be empty", nameof(peerUri));
            if (body == null || body.Length == 0) throw new ArgumentException("Message body cannot be empty", nameof(body));
            if (string.IsNullOrEmpty(contentType)) throw new ArgumentException("Content type cannot be empty", nameof(contentType));

            int rc = SipNativeAbi.sip_account_send_message(
                _accountHandle,
                SipNativeAbi.ToUtf8Z(peerUri),
                SipNativeAbi.ToUtf8Z(contentType),
                body,
                (UIntPtr)body.Length,
                out ulong messageId);
            if (rc != 0)
            {
                throw new InvalidOperationException($"Failed to send message to {peerUri} (error {rc})");
            }
            return messageId;
        }

        internal void ThrowIfDisposed()
        {
            if (_disposed || _accountHandle == IntPtr.Zero)
            {
                throw new ObjectDisposedException(nameof(SipAccount));
            }
        }

        public void Dispose()
        {
            _disposed = true;
            IntPtr handle = Interlocked.Exchange(ref _accountHandle, IntPtr.Zero);
            if (handle != IntPtr.Zero)
            {
                _client.RemoveAccountEntry(handle);
                if (_ownsHandle)
                {
                    SipNativeAbi.sip_account_destroy(handle);
                }
            }
        }
    }
}

using System;
using System.Runtime.InteropServices;

namespace SipSdk.Native
{
    [StructLayout(LayoutKind.Sequential)]
    public struct SipVideoFrameNative
    {
        public int Width;
        public int Height;
        public IntPtr Y;
        public IntPtr U;
        public IntPtr V;
        public int YStride;
        public int UStride;
        public int VStride;
        public ulong TimestampNs;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct SipCallbacksNative
    {
        public IntPtr DispatchToMain;
        public IntPtr OnRegState;
        public IntPtr OnIncomingCall;
        public IntPtr OnCallState;
        public IntPtr OnCallMedia;
        public IntPtr OnDtmfDigit;
        public IntPtr PrepareAudioSession;
        public IntPtr ReleaseAudioSession;
        public IntPtr OnCallStats;
        public IntPtr OnTransferStatus;
        public IntPtr OnTransferRequest;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    public struct SipCallStatsNative
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string AudioCodec;
        public uint AudioBitrateKbps;
        public float AudioLossRate;
        public uint AudioJitterMs;
        public uint AudioRttMs;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string VideoCodec;
        public uint VideoBitrateKbps;
        public float VideoLossRate;
        public uint VideoJitterMs;
        public uint VideoFps;
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void NativeCallStatsCallback(IntPtr call, IntPtr stats, IntPtr userData);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void NativeTransferStatusCallback(IntPtr call, int code, [MarshalAs(UnmanagedType.LPStr)] string reason, IntPtr userData);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void NativeTransferRequestCallback(IntPtr call, [MarshalAs(UnmanagedType.LPStr)] string referToUri, [MarshalAs(UnmanagedType.I1)] bool hasReplaces, IntPtr userData);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void NativeRegStateCallback(IntPtr client, int state, int code, [MarshalAs(UnmanagedType.LPStr)] string reason, IntPtr userData);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void NativeIncomingCallCallback(IntPtr client, IntPtr call, IntPtr userData);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void NativeCallStateCallback(IntPtr call, int state, int code, [MarshalAs(UnmanagedType.LPStr)] string reason, IntPtr userData);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void NativeCallMediaCallback(IntPtr call, int width, int height, [MarshalAs(UnmanagedType.I1)] bool onHold, [MarshalAs(UnmanagedType.I1)] bool isMuted, IntPtr userData);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void NativeDtmfDigitCallback(IntPtr call, [MarshalAs(UnmanagedType.LPStr)] string digit, IntPtr userData);

    [StructLayout(LayoutKind.Sequential)]
    public struct SipMessageCallbacksNative
    {
        public IntPtr OnMessageReceived;
        public IntPtr OnMessageStatus;
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void NativeMessageReceivedCallback(IntPtr client, IntPtr fromUri, IntPtr contentType, IntPtr body, UIntPtr bodyLen, IntPtr userData);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void NativeMessageStatusCallback(IntPtr client, ulong messageId, int status, int code, IntPtr reason, IntPtr userData);

    internal static class SipNativeAbi
    {
        public const string LibraryName = "sipcore";

        static SipNativeAbi()
        {
            NativeLibraryResolver.EnsureInitialized();
        }

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr sip_client_create(ref SipCallbacksNative callbacks, IntPtr userData);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int sip_client_init(IntPtr client, [MarshalAs(UnmanagedType.LPStr)] string? configPath);

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
        public struct SipAccountConfigNative
        {
            [MarshalAs(UnmanagedType.LPStr)] public string User;
            [MarshalAs(UnmanagedType.LPStr)] public string Domain;
            [MarshalAs(UnmanagedType.LPStr)] public string? Pwd;
            [MarshalAs(UnmanagedType.LPStr)] public string Transport;
            public int Port;
            [MarshalAs(UnmanagedType.LPStr)] public string? DisplayName;
            [MarshalAs(UnmanagedType.LPStr)] public string? OutboundProxy;
            [MarshalAs(UnmanagedType.LPStr)] public string? StunServer;
            [MarshalAs(UnmanagedType.LPStr)] public string? StunUser;
            [MarshalAs(UnmanagedType.LPStr)] public string? StunPass;
            [MarshalAs(UnmanagedType.LPStr)] public string? MediaNat;
            public uint RegInterval;
            [MarshalAs(UnmanagedType.LPStr)] public string? TlsCaFile;
            [MarshalAs(UnmanagedType.I1)] public bool VerifyTlsCert;
            [MarshalAs(UnmanagedType.LPStr)] public string? MediaEnc;
        }

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int sip_client_register_account(IntPtr client, ref SipAccountConfigNative config);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int sip_client_register(
            IntPtr client,
            [MarshalAs(UnmanagedType.LPStr)] string user,
            [MarshalAs(UnmanagedType.LPStr)] string domain,
            [MarshalAs(UnmanagedType.LPStr)] string pwd,
            [MarshalAs(UnmanagedType.LPStr)] string transport,
            int port);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int sip_client_unregister(IntPtr client);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool sip_client_is_registered(IntPtr client);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void sip_client_destroy(IntPtr client);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr sip_call_make(
            IntPtr client,
            [MarshalAs(UnmanagedType.LPStr)] string targetUri,
            [MarshalAs(UnmanagedType.I1)] bool withVideo);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int sip_call_answer(IntPtr call, [MarshalAs(UnmanagedType.I1)] bool withVideo);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int sip_call_hangup(IntPtr call, int code, [MarshalAs(UnmanagedType.LPStr)] string? reason);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int sip_call_set_hold(IntPtr call, [MarshalAs(UnmanagedType.I1)] bool hold);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int sip_call_set_muted(IntPtr call, [MarshalAs(UnmanagedType.I1)] bool muted);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int sip_call_send_dtmf(IntPtr call, [MarshalAs(UnmanagedType.LPStr)] string digits);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int sip_call_transfer(IntPtr call, [MarshalAs(UnmanagedType.LPStr)] string targetUri);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int sip_call_attended_transfer(IntPtr call, IntPtr consultCall);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int sip_call_accept_transfer(IntPtr call, [MarshalAs(UnmanagedType.I1)] bool accept);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int sip_client_set_mic_muted(IntPtr client, [MarshalAs(UnmanagedType.I1)] bool muted);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool sip_client_is_mic_muted(IntPtr client);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr sip_call_get_id(IntPtr call);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr sip_call_get_remote_uri(IntPtr call);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr sip_call_get_peer_display_name(IntPtr call);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool sip_call_has_video(IntPtr call);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool sip_call_is_outgoing(IntPtr call);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool sip_call_is_on_hold(IntPtr call);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool sip_call_is_muted(IntPtr call);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool sip_call_is_closed(IntPtr call);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool sip_call_is_secure(IntPtr call);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern uint sip_call_get_duration(IntPtr call);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int sip_call_get_stats(IntPtr call, out SipCallStatsNative stats);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void sip_call_destroy(IntPtr call);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int sip_call_request_keyframe(IntPtr call);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int sip_client_switch_camera(IntPtr client);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int sip_client_set_camera(IntPtr client, int position);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int sip_client_set_local_video_enabled(IntPtr client, [MarshalAs(UnmanagedType.I1)] bool enabled);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool sip_client_is_local_video_enabled(IntPtr client);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int sip_client_start_local_preview(IntPtr client);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int sip_client_stop_local_preview(IntPtr client);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool sip_client_is_local_preview_active(IntPtr client);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int sip_client_set_message_callbacks(IntPtr client, ref SipMessageCallbacksNative callbacks);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int sip_client_send_message(
            IntPtr client,
            byte[] peerUriUtf8Z,
            byte[] contentTypeUtf8Z,
            byte[] body,
            UIntPtr bodyLen,
            out ulong messageId);

        /// <summary>NUL-terminated UTF-8 bytes (netstandard2.0 has no LPUTF8Str marshalling).</summary>
        public static byte[] ToUtf8Z(string value)
        {
            int len = System.Text.Encoding.UTF8.GetByteCount(value);
            byte[] bytes = new byte[len + 1];
            System.Text.Encoding.UTF8.GetBytes(value, 0, value.Length, bytes, 0);
            return bytes;
        }

        /// <summary>Reads a NUL-terminated UTF-8 string; null for a null pointer.</summary>
        public static string? PtrToStringUtf8(IntPtr ptr)
        {
            if (ptr == IntPtr.Zero) return null;
            int len = 0;
            while (Marshal.ReadByte(ptr, len) != 0) len++;
            byte[] bytes = new byte[len];
            Marshal.Copy(ptr, bytes, 0, len);
            return System.Text.Encoding.UTF8.GetString(bytes);
        }
    }
}

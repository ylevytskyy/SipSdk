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
    }

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
        public static extern uint sip_call_get_duration(IntPtr call);

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
    }
}

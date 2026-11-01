using System;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;

namespace SipSdk.Native
{
    internal static class NativeLibraryResolver
    {
        private static bool _initialized = false;
        private static readonly object _lock = new object();

        public static void EnsureInitialized()
        {
            if (_initialized) return;

            lock (_lock)
            {
                if (_initialized) return;

#if NET5_0_OR_GREATER || NETCOREAPP3_0_OR_GREATER
                NativeLibrary.SetDllImportResolver(typeof(NativeLibraryResolver).Assembly, DllImportResolver);
#endif
                _initialized = true;
            }
        }

#if NET5_0_OR_GREATER || NETCOREAPP3_0_OR_GREATER
        private static IntPtr DllImportResolver(string libraryName, Assembly assembly, DllImportSearchPath? searchPath)
        {
            if (libraryName != SipNativeAbi.LibraryName)
            {
                return IntPtr.Zero;
            }

            string[] candidateNames;
            if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
            {
                candidateNames = new[] { "libsipcore.dll", "sipcore.dll" };
            }
            else if (RuntimeInformation.IsOSPlatform(OSPlatform.OSX))
            {
                candidateNames = new[] { "libsipcore.dylib", "sipcore.dylib" };
            }
            else
            {
                candidateNames = new[] { "libsipcore.so", "sipcore.so" };
            }

            string? envExplicit = Environment.GetEnvironmentVariable("SIP_CORE_LIB_PATH");
            if (!string.IsNullOrEmpty(envExplicit) && File.Exists(envExplicit))
            {
                if (NativeLibrary.TryLoad(envExplicit, out IntPtr handle))
                {
                    return handle;
                }
            }

            var searchDirs = new System.Collections.Generic.List<string>();

            string? envDir = Environment.GetEnvironmentVariable("SIP_CORE_LIB_DIR");
            if (!string.IsNullOrEmpty(envDir) && Directory.Exists(envDir))
            {
                searchDirs.Add(envDir);
            }

            string baseDir = AppDomain.CurrentDomain.BaseDirectory;
            searchDirs.Add(baseDir);
            searchDirs.Add(Path.Combine(baseDir, "runtimes", GetRuntimeIdentifier(), "native"));

            // Traverse parent directories to discover .build locations from repo root
            DirectoryInfo? curr = new DirectoryInfo(baseDir);
            for (int i = 0; i < 10 && curr != null; i++)
            {
                string winPath = Path.Combine(curr.FullName, ".build", "windows");
                if (Directory.Exists(winPath)) searchDirs.Add(winPath);

                string deskCorePath = Path.Combine(curr.FullName, ".build", "desktop", "core");
                if (Directory.Exists(deskCorePath)) searchDirs.Add(deskCorePath);

                curr = curr.Parent;
            }

            foreach (var dir in searchDirs)
            {
                if (!Directory.Exists(dir)) continue;

                foreach (var name in candidateNames)
                {
                    string candidatePath = Path.Combine(dir, name);
                    if (File.Exists(candidatePath))
                    {
                        if (NativeLibrary.TryLoad(candidatePath, out IntPtr handle))
                        {
                            return handle;
                        }
                    }
                }
            }

            // Fall back to default runtime search
            foreach (var name in candidateNames)
            {
                if (NativeLibrary.TryLoad(name, assembly, searchPath, out IntPtr handle))
                {
                    return handle;
                }
            }

            return IntPtr.Zero;
        }

        private static string GetRuntimeIdentifier()
        {
            string arch = RuntimeInformation.ProcessArchitecture switch
            {
                Architecture.Arm64 => "arm64",
                Architecture.X64 => "x64",
                Architecture.X86 => "x86",
                _ => "x64"
            };

            if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows)) return $"win-{arch}";
            if (RuntimeInformation.IsOSPlatform(OSPlatform.OSX)) return $"osx-{arch}";
            return $"linux-{arch}";
        }
#endif
    }
}

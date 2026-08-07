#pragma once

#include <string>

namespace lwi::forge
{

// The two manifest variants the forge stamps into a stub, chosen by
// install.elevation in the config.
//
// Everything in them is load-bearing:
//
// - An explicit requestedExecutionLevel. Without one, UAC Installer Detection
//   auto-elevates 32-bit executables whose filename or version resource looks
//   like a setup program. That heuristic cannot reach an x64 image, but its
//   absence is a silent regression if the stub ever ships as 32-bit.
// - Common Controls 6.0.0.0. Without it the wizard renders in the Windows 95
//   style, which reads as counterfeit rather than merely dated, and
//   BCM_SETSHIELD does not work.
// - PerMonitorV2 plus the legacy dpiAware tag, so the window is not bitmap
//   scaled into a blur on the high-DPI display most machines now have.
// - activeCodePage UTF-8.
// - The Windows 10 and 11 supportedOS GUIDs. Without them the OS reports a
//   compatibility version of 6.2 and every version check answers wrongly.
//
// There is deliberately no longPathAware. It is necessary but not sufficient:
// HKLM\SYSTEM\CurrentControlSet\Control\FileSystem\LongPathsEnabled must also
// be 1 on the target machine, and that is not something an installer can assume
// about a customer box. The stub prefixes paths with \\?\ instead.

namespace detail
{

inline const std::string kManifestPrefix =
    R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3">
    <security>
      <requestedPrivileges>
        <requestedExecutionLevel level=")";

inline const std::string kManifestSuffix =
    R"(" uiAccess="false"/>
      </requestedPrivileges>
    </security>
  </trustInfo>

  <dependency>
    <dependentAssembly>
      <assemblyIdentity
          type="win32"
          name="Microsoft.Windows.Common-Controls"
          version="6.0.0.0"
          processorArchitecture="*"
          publicKeyToken="6595b64144ccf1df"
          language="*"/>
    </dependentAssembly>
  </dependency>

  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">PerMonitorV2</dpiAwareness>
      <dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true/pm</dpiAware>
      <activeCodePage xmlns="http://schemas.microsoft.com/SMI/2019/WindowsSettings">UTF-8</activeCodePage>
    </windowsSettings>
  </application>

  <compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1">
    <application>
      <supportedOS Id="{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"/>
      <supportedOS Id="{1f676c76-80e1-4239-95bb-83d0f6d0da78}"/>
      <supportedOS Id="{4a2f28e3-53b9-4441-ba9c-d69d4a4a6e38}"/>
    </application>
  </compatibility>
</assembly>
)";

} // namespace detail

/// install.elevation = "on-demand". The wizard runs unelevated so the user sees
/// the license and any warnings before a UAC prompt, and so per-user work can
/// happen in the invoking user's token. The stub does not elevate itself, so a
/// machine-scope install picking this has to be launched elevated already.
inline const std::string kManifestAsInvoker =
    detail::kManifestPrefix + "asInvoker" + detail::kManifestSuffix;

/// The default, and install.elevation = "required". Costs a UAC prompt before
/// any UI appears, and buys an install that can write Program Files and HKLM
/// without the author wiring up elevation themselves.
inline const std::string kManifestRequireAdministrator =
    detail::kManifestPrefix + "requireAdministrator" + detail::kManifestSuffix;

} // namespace lwi::forge

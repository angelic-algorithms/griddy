// macOS only. Compiled with ARC.
#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>
#import <Security/Security.h>

#include "CaptureSource.h"

namespace griddy
{

/*  Why this file exists

    A plugin is a library loaded into the DAW's process. macOS privacy rules
    (TCC) attribute camera use to that process, so they look at the DAW's
    Info.plist and entitlements, not ours. If the DAW never declared
    NSCameraUsageDescription, or it uses the hardened runtime without the
    com.apple.security.device.camera entitlement, macOS terminates the DAW the
    moment anything in it touches the camera. Losing someone's unsaved session
    is the worst thing a plugin can do, so we check first and refuse politely.
*/

static bool processHasEntitlement (CFStringRef entitlement)
{
    SecTaskRef task = SecTaskCreateFromSelf (kCFAllocatorDefault);

    if (task == nullptr)
        return false;

    CFTypeRef value = SecTaskCopyValueForEntitlement (task, entitlement, nullptr);
    const bool granted = value != nullptr && CFGetTypeID (value) == CFBooleanGetTypeID() && CFBooleanGetValue ((CFBooleanRef) value);

    if (value != nullptr)
        CFRelease (value);

    CFRelease (task);
    return granted;
}

static bool processUsesHardenedRuntime()
{
    SecCodeRef code = nullptr;

    if (SecCodeCopySelf (kSecCSDefaultFlags, &code) != errSecSuccess || code == nullptr)
        return false;

    SecStaticCodeRef staticCode = nullptr;
    bool hardened = false;

    if (SecCodeCopyStaticCode (code, kSecCSDefaultFlags, &staticCode) == errSecSuccess && staticCode != nullptr)
    {
        CFDictionaryRef signingInfo = nullptr;

        if (SecCodeCopySigningInformation (staticCode, kSecCSSigningInformation, &signingInfo) == errSecSuccess && signingInfo != nullptr)
        {
            if (auto flags = (CFNumberRef) CFDictionaryGetValue (signingInfo, kSecCodeInfoFlags))
            {
                uint32_t value = 0;
                CFNumberGetValue (flags, kCFNumberSInt32Type, &value);
                hardened = (value & 0x10000) != 0; // kSecCodeSignatureRuntime
            }

            CFRelease (signingInfo);
        }

        CFRelease (staticCode);
    }

    CFRelease (code);
    return hardened;
}

bool hostAllowsCameraAccess (juce::String& reason)
{
    NSBundle* host = [NSBundle mainBundle];
    const auto hostName = juce::String ([[[host bundleURL] lastPathComponent] UTF8String]);

    if ([host objectForInfoDictionaryKey: @"NSCameraUsageDescription"] == nil)
    {
        reason = hostName + " doesn't ask macOS for camera access, so cameras can't be used inside it. "
                            "Use the Griddy standalone app for cameras.";
        return false;
    }

    if (processUsesHardenedRuntime() && ! processHasEntitlement (CFSTR ("com.apple.security.device.camera")))
    {
        reason = hostName + " isn't signed with the camera entitlement, so macOS would close it if a plugin "
                            "opened a camera. Use the Griddy standalone app for cameras.";
        return false;
    }

    switch ([AVCaptureDevice authorizationStatusForMediaType: AVMediaTypeVideo])
    {
        case AVAuthorizationStatusDenied:
            reason = "Camera access for " + hostName + " is turned off in System Settings > Privacy & Security > Camera.";
            return false;

        case AVAuthorizationStatusRestricted:
            reason = "Camera access is restricted on this Mac (for example by a device management profile).";
            return false;

        case AVAuthorizationStatusAuthorized:
        case AVAuthorizationStatusNotDetermined: // macOS will ask the first time a camera opens
        default:
            reason = {};
            return true;
    }
}

} // namespace griddy

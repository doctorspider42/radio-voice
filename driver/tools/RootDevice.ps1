<#
    Creating the root-enumerated device without devcon.

    Dot-source this, do not run it.

    There is no hardware to enumerate a virtual audio cable, so the device node
    has to be created by hand. `devcon install` does that, and devcon ships with
    the WDK - which is exactly what a machine running the installer does not
    have.

    So the same three SetupAPI calls devcon makes are made here directly. In
    order:

      1. SetupDiCreateDeviceInfo   - a device node of class MEDIA
      2. SetupDiSetDeviceRegistryProperty(SPDRP_HARDWAREID)
                                   - which driver it is asking for
      3. SetupDiCallClassInstaller(DIF_REGISTERDEVICE)
                                   - commit it to the system
      4. UpdateDriverForPlugAndPlayDevices
                                   - match the INF to it and install

    Step 4 alone is what an *update* needs: the node already exists, and only
    the driver behind it changes.

    Add-Type compiles against the .NET Framework that ships in the box, so this
    needs nothing installed. It is Windows PowerShell 5.1 only, which is the
    PowerShell every supported Windows has.
#>

$script:RvRootDeviceTypeName = 'RvRootDevice'

function Initialize-RootDeviceType {
    if ($script:RvRootDeviceTypeName -as [type]) { return }

    Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

public static class RvRootDevice
{
    private const int DICD_GENERATE_ID   = 0x00000001;
    private const int SPDRP_HARDWAREID   = 0x00000001;
    private const int DIF_REGISTERDEVICE = 0x00000019;
    private const int INSTALLFLAG_FORCE  = 0x00000001;

    [StructLayout(LayoutKind.Sequential)]
    private struct SP_DEVINFO_DATA
    {
        public int    cbSize;
        public Guid   ClassGuid;
        public int    DevInst;
        public IntPtr Reserved;
    }

    [DllImport("setupapi.dll", SetLastError = true)]
    private static extern IntPtr SetupDiCreateDeviceInfoList(
        ref Guid ClassGuid, IntPtr hwndParent);

    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern bool SetupDiCreateDeviceInfoW(
        IntPtr DeviceInfoSet, string DeviceName, ref Guid ClassGuid,
        string DeviceDescription, IntPtr hwndParent, int CreationFlags,
        ref SP_DEVINFO_DATA DeviceInfoData);

    [DllImport("setupapi.dll", SetLastError = true)]
    private static extern bool SetupDiSetDeviceRegistryPropertyW(
        IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData, int Property,
        byte[] PropertyBuffer, int PropertyBufferSize);

    [DllImport("setupapi.dll", SetLastError = true)]
    private static extern bool SetupDiCallClassInstaller(
        int InstallFunction, IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData);

    [DllImport("setupapi.dll", SetLastError = true)]
    private static extern bool SetupDiDestroyDeviceInfoList(IntPtr DeviceInfoSet);

    [DllImport("newdev.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern bool UpdateDriverForPlugAndPlayDevicesW(
        IntPtr hwndParent, string HardwareId, string FullInfPath, int InstallFlags,
        out bool bRebootRequired);

    private static void Fail(string what)
    {
        throw new Win32Exception(Marshal.GetLastWin32Error(), what + " failed");
    }

    // A REG_MULTI_SZ holding one string: the string, its terminator, and the
    // empty string that ends the list.
    private static byte[] MultiSz(string value)
    {
        return Encoding.Unicode.GetBytes(value + "\0\0");
    }

    /// <summary>Creates the device node. Returns true if a reboot is needed.</summary>
    public static bool Create(string className, Guid classGuid, string hardwareId,
                              string infPath)
    {
        IntPtr set = SetupDiCreateDeviceInfoList(ref classGuid, IntPtr.Zero);
        if (set == IntPtr.Zero || set == new IntPtr(-1))
            Fail("SetupDiCreateDeviceInfoList");

        try
        {
            SP_DEVINFO_DATA info = new SP_DEVINFO_DATA();
            info.cbSize = Marshal.SizeOf(typeof(SP_DEVINFO_DATA));

            if (!SetupDiCreateDeviceInfoW(set, className, ref classGuid, null,
                                          IntPtr.Zero, DICD_GENERATE_ID, ref info))
                Fail("SetupDiCreateDeviceInfo");

            byte[] hwid = MultiSz(hardwareId);
            if (!SetupDiSetDeviceRegistryPropertyW(set, ref info, SPDRP_HARDWAREID,
                                                   hwid, hwid.Length))
                Fail("SetupDiSetDeviceRegistryProperty");

            if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, ref info))
                Fail("SetupDiCallClassInstaller(DIF_REGISTERDEVICE)");
        }
        finally
        {
            SetupDiDestroyDeviceInfoList(set);
        }

        return Update(hardwareId, infPath);
    }

    /// <summary>Installs the INF onto an existing node. True if a reboot is needed.</summary>
    public static bool Update(string hardwareId, string infPath)
    {
        bool reboot;
        if (!UpdateDriverForPlugAndPlayDevicesW(IntPtr.Zero, hardwareId, infPath,
                                                INSTALLFLAG_FORCE, out reboot))
            Fail("UpdateDriverForPlugAndPlayDevices");

        return reboot;
    }
}
'@
}

<#
.SYNOPSIS
    Creates or updates the RadioVoice root-enumerated device.

.OUTPUTS
    $true when Windows wants a reboot to finish.
#>
function Install-RootDevice {
    param(
        [Parameter(Mandatory)][string] $InfPath,
        [string] $HardwareId = 'root\RadioVoiceAudio',
        # From the INF's [Version] section. Hard-coded rather than read back out
        # of the INF because it is a property of this driver, not of the file.
        [string] $ClassName  = 'MEDIA',
        [guid]   $ClassGuid  = '4d36e96c-e325-11ce-bfc1-08002be10318',
        [switch] $Update
    )

    Initialize-RootDeviceType

    $InfPath = (Resolve-Path $InfPath).Path

    if ($Update) {
        return [RvRootDevice]::Update($HardwareId, $InfPath)
    }
    return [RvRootDevice]::Create($ClassName, $ClassGuid, $HardwareId, $InfPath)
}

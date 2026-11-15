# Installs Magic & Mayhem from the original disc without running its setup.exe
# (a 16-bit InstallShield launcher that 64-bit Windows can't run).
#
# Usage: drag the .iso (or the disc drive / a folder with the disc contents)
# onto "Install from ISO.bat", or run:
#   powershell -ExecutionPolicy Bypass -File install-from-iso.ps1 [-Source <iso|folder>] [-Dest <folder>]
#
# What it does, in the folder this script is in (or -Dest):
#   - unpacks data1.cab the way the installer would lay it out
#   - copies REALMS\ and FMV\ from the disc
#   - writes CDROMDrive.cfg so the game reads those from its own folder
#   - creates the empty Save folder for saved games
# Files that already exist are left alone, so the patch's CFG\prefs.cfg and
# Sounds\Sounds.ini (and your saves) are kept if you extracted the zip first.

param(
    [string]$Source,
    [string]$Dest = $PSScriptRoot
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Text;

public static class IsCab5 {
    static byte[] d;
    static uint U32(long o) { return BitConverter.ToUInt32(d, (int)o); }
    static ushort U16(long o) { return BitConverter.ToUInt16(d, (int)o); }
    static Encoding enc;
    static string Str(long o) {
        int e = (int)o; while (d[e] != 0) e++;
        return enc.GetString(d, (int)o, e - (int)o);
    }

    // groupDirs: InstallShield file group name -> folder under the install dir.
    // Returns {written, skipped}.
    public static int[] Extract(string cab, string dest, Dictionary<string, string> groupDirs) {
        try { enc = Encoding.GetEncoding(936); } catch { enc = Encoding.Default; }
        d = File.ReadAllBytes(cab);
        long b = U32(12);
        long ft = U32(b + 0xc);
        int dircnt = (int)U32(b + 0x1c), filecnt = (int)U32(b + 0x28);

        // file groups: 71 hash slots of linked lists; each covers a file index range
        var gName = new List<string>(); var gFirst = new List<uint>(); var gLast = new List<uint>();
        for (int slot = 0; slot < 71; slot++) {
            long node = U32(b + 0x3e + 4 * slot);
            while (node != 0) {
                string name = Str(b + U32(b + node));
                long desc = U32(b + node + 4);
                gName.Add(name); gFirst.Add(U32(b + desc + 0x4c)); gLast.Add(U32(b + desc + 0x50));
                node = U32(b + node + 8);
            }
        }

        var dirs = new string[dircnt];
        for (int i = 0; i < dircnt; i++) dirs[i] = Str(b + ft + U32(b + ft + 4 * i));

        int written = 0, skipped = 0;
        for (int i = 0; i < filecnt; i++) {
            long p = b + ft + U32(b + ft + 4 * (dircnt + i));
            uint nameOff = U32(p), dirIdx = U32(p + 4);
            ushort flags = U16(p + 8);
            uint size = U32(p + 10), csize = U32(p + 14), off = U32(p + 38);
            if ((flags & 8) != 0 || off == 0) continue;   // not stored in this cab

            string group = null;
            for (int g = 0; g < gName.Count; g++)
                if (gFirst[g] <= i && i <= gLast[g]) { group = gName[g]; break; }
            string gdir;
            if (group == null || !groupDirs.TryGetValue(group, out gdir))
                throw new Exception("data1.cab has a file group this script doesn't know: '" + group +
                                    "'. This disc may be a different version than the one it was written for.");

            string rel = Path.Combine(gdir, dirs[dirIdx].Trim('\\'), Str(b + ft + nameOff));
            string outPath = Path.Combine(dest, rel);
            if (File.Exists(outPath)) { skipped++; continue; }
            Directory.CreateDirectory(Path.GetDirectoryName(outPath));

            byte[] data;
            if ((flags & 4) != 0) {
                // compressed: sequence of [u16 length][raw deflate block]
                var ms = new MemoryStream((int)size);
                long q = off, end = off + csize;
                while (q < end) {
                    int n = U16(q); q += 2;
                    using (var ds = new DeflateStream(new MemoryStream(d, (int)q, n), CompressionMode.Decompress))
                        ds.CopyTo(ms);
                    q += n;
                }
                data = ms.ToArray();
            } else {
                data = new byte[size];
                Array.Copy(d, off, data, 0, size);
            }
            if (data.Length != size)
                throw new Exception(rel + ": unpacked " + data.Length + " bytes, expected " + size);

            File.WriteAllBytes(outPath + ".part", data);
            File.Move(outPath + ".part", outPath);
            written++;
        }
        d = null;
        return new int[] { written, skipped };
    }
}
'@

# Where the original installer puts each file group (from the disc's data1.cab).
$groupDirs = New-Object 'System.Collections.Generic.Dictionary[string,string]'
@{
    'Program DLLs'                 = ''
    'Program Executable Files'     = ''
    'Shared DLLs'                  = ''
    'Readme Files'                 = ''
    'Configuration Files'          = 'CFG'
    'Bitmaps'                      = 'Bitmaps'
    'Sound Files'                  = 'Sounds'
    'Sprite Files'                 = 'Sprites'
    'Creature Files'               = 'Creatures'
    'Interface Screens - Low Res'  = 'Interface'
    'Interface Screens - High Res' = 'Interface'
    'Interface Screens - Generic'  = 'Interface'
    'Wizard Files'                 = 'Wizards'
    'Realms'                       = 'Realms'
    'AI Experience Files'          = 'AI'
    'Cursors'                      = 'Cursors'
    'Text Files'                   = 'Text'
    'Portraits - Low Res'          = 'Portraits\640x480'
    'Portraits - High Res'         = 'Portraits\800x600'
    'SpeechBox - Low Res'          = 'SpeechBox\640x480'
    'SpeechBox - High Res'         = 'SpeechBox\800x600'
}.GetEnumerator() | ForEach-Object { $groupDirs[$_.Key] = $_.Value }

if (-not $Source) {
    Add-Type -AssemblyName System.Windows.Forms
    $dlg = New-Object System.Windows.Forms.OpenFileDialog
    $dlg.Title = 'Select the Magic & Mayhem disc image'
    $dlg.Filter = 'Disc image (*.iso)|*.iso|All files (*.*)|*.*'
    if ($dlg.ShowDialog() -ne 'OK') { Write-Host 'Cancelled.'; exit 1 }
    $Source = $dlg.FileName
}
$Source = $Source.Trim('"')
$Dest = (New-Item -ItemType Directory -Force $Dest).FullName

$mounted = $null
try {
    if (Test-Path -LiteralPath $Source -PathType Leaf) {
        Write-Host "Mounting $Source"
        $mounted = (Resolve-Path -LiteralPath $Source).ProviderPath
        $vol = Mount-DiskImage -ImagePath $mounted -StorageType ISO -PassThru | Get-Volume
        $disc = "$($vol.DriveLetter):\"
    } else {
        $disc = $Source
    }
    $cab = Join-Path $disc 'data1.cab'
    if (-not (Test-Path -LiteralPath $cab)) { throw "No data1.cab in $disc. Is this the Magic & Mayhem disc?" }

    Write-Host "Unpacking game files into $Dest (takes a minute)"
    $r = [IsCab5]::Extract($cab, $Dest, $groupDirs)
    Write-Host "  $($r[0]) files written, $($r[1]) already there"

    foreach ($dir in 'REALMS', 'FMV') {
        $src = Join-Path $disc $dir
        if (-not (Test-Path -LiteralPath $src)) { throw "No $dir folder on the disc." }
        Write-Host "Copying $dir from the disc"
        $n = 0
        Get-ChildItem -LiteralPath $src -Recurse -File | ForEach-Object {
            $to = Join-Path $Dest ($dir + $_.FullName.Substring($src.Length))
            if (-not (Test-Path -LiteralPath $to)) {
                New-Item -ItemType Directory -Force (Split-Path $to) | Out-Null
                Copy-Item -LiteralPath $_.FullName $to
                $n++
            }
        }
        Write-Host "  $n files copied"
    }
} finally {
    if ($mounted) { Dismount-DiskImage -ImagePath $mounted | Out-Null }
}

# The game looks for REALMS\ and FMV\ on the drive named here; point it at its own folder.
[IO.File]::WriteAllText((Join-Path $Dest 'CDROMDrive.cfg'),
    "[CDROM Drive Letter With Colon]`r`nCDROMDrive=.\`r`n")

# Saved games go here; the original installer creates it empty, and saving fails without it.
New-Item -ItemType Directory -Force (Join-Path $Dest 'Save') | Out-Null

Write-Host ''
if (Test-Path -LiteralPath (Join-Path $Dest 'ddraw.dll')) {
    Write-Host "Done. Run Chaos.exe in $Dest"
} else {
    Write-Host "Game files done. Now extract the Magic & Mayhem HD release zip into $Dest, then run Chaos.exe."
}

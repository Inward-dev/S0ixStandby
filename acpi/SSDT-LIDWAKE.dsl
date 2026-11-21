/*
 * SSDT-LIDWAKE: let the lid wake the machine from Deep Idle through the
 * embedded controller's GPE. Experimental, per machine.
 *
 * On many laptops opening the lid is an EC event (_Qxx -> Notify(LID)) on the
 * EC's GPE, but the lid device has no _PRW, so macOS never arms that GPE for
 * sleep and only the power button wakes. AppleACPIPlatform builds the wake
 * GPE set from the devices' _PRW (reported for a ThinkPad E16 in #7), so the
 * GPE has to come from a _PRW: a bit set by hand at the quiesce action is
 * cleared again.
 *
 * Side effect: every EC event (battery, AC, charge level, thermal) can then
 * wake the machine. On AC that may mean a dark wake every minute while
 * charging. Check "Since boot ... dark wakes" in s0ix-status.sh, and test on
 * battery first.
 *
 * HP EliteBook 840 G8 result (2026-11-21): lid wake works (battery and AC)
 * (clamshell sleep, lid open wakes, GPE 0x6E armed during the halt, no extra
 * wake on close). On AC: with the lid closed an EC event dark-woke it after
 * 31 s and the re-sleep hung GFX0 (the known AC framebuffer hang); another
 * AC lid wake ended in "Darkwake Exit Failure". Do not use it on AC there.
 * On battery it hung too: overnight, a lid wake became a 45 s dark wake
 * ("due to LID/", unlike RTC dark wakes of 0 s) and the re-sleep after it
 * hung GFX0. Not recommended on the 840 G8 at all. (_PRW is read once at
 * boot, so it can't follow the power source.)
 *
 * Adapt per machine:
 *   - the lid path (\_SB.LID here; HP EliteBook 840 G8);
 *   - the EC GPE (0x6E here: the EC's _GPE, shown as "EC wake" in s0ix-status.sh);
 *   - if the lid device already has a _PRW, rename it to XPRW with an OpenCore
 *     ACPI patch first, otherwise this table is rejected (duplicate _PRW).
 */
DefinitionBlock ("", "SSDT", 2, "S0IX", "LIDWAKE", 0x00000000)
{
    External (_SB_.LID_, DeviceObj)

    Scope (\_SB.LID)
    {
        Method (_PRW, 0, NotSerialized)  // _PRW: Power Resources for Wake
        {
            If (_OSI ("Darwin"))
            {
                Return (Package (0x02) { 0x6E, 0x03 })
            }

            Return (Package (0x02) { 0x6E, Zero })
        }
    }
}

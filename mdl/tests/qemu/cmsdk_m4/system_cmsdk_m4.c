/* Minimal SystemInit() for the QEMU target -- unlike AT32's, there is no
 * real clock tree to configure (QEMU's Cortex-M4 model just runs), so
 * this is a no-op stub, present only because startup_cmsdk_m4.s calls
 * it unconditionally (matching the vendor startup pattern it was
 * trimmed from). */
void SystemInit(void)
{
}

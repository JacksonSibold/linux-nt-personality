/*
 * vcruntime140_1.c - vcruntime140_1.dll holds __CxxFrameHandler4 in the Microsoft runtime; here the handler is part of
 * vcruntime140.dll and this DLL forwards its one export there (see vcruntime140_1.def).
 */
#ifdef __MINGW32__
int __stdcall DllMainCRTStartup(void *instance, unsigned long reason, void *reserved)
{
    (void)instance;
    (void)reason;
    (void)reserved;
    return 1;
}
#endif

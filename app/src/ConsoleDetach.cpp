// AICore-C: the exe is a console app (the CLI and the gates read its stdout). Opened from the Explorer or a
// shortcut, Windows gives it a console of its own: hide that one and let it go, so only the window shows.
#ifdef _WIN32
 #define WIN32_LEAN_AND_MEAN
 #define NOMINMAX
 #include <windows.h>
#endif

namespace aicore
{
void detachOwnConsole()
{
#ifdef _WIN32
    DWORD ids[4];
    if (GetConsoleProcessList (ids, 4) == 1) // nobody else on this console: it was made just for us
    {
        if (auto w = GetConsoleWindow())
            ShowWindow (w, SW_HIDE);
        FreeConsole();
    }
#endif
}
} // namespace aicore

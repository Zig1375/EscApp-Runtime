/* Directory change notifications the way programs rely on them (tests/change/run.sh).
 *
 * The Epic Games Launcher waits with ReadDirectoryChangesW for a file that a second copy of itself writes into a
 * watched folder; without notifications it gives the install up. The probe does the same across two processes,
 * then checks the records one by one: added, modified, renamed (old and new name together), removed, a change in a
 * subfolder (seen with subtree, not without) and FindFirstChangeNotification. Each check prints PASS or FAIL. */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static int failures;
static WCHAR root[MAX_PATH];

static void check( int ok, const char *what )
{
    printf( "%s %s\n", ok ? "PASS" : "FAIL", what );
    if (!ok) failures++;
}

struct watch
{
    HANDLE dir;
    OVERLAPPED ov;
    DWORD buffer[4096];
};

static void watch_start( struct watch *w, const WCHAR *path, BOOL subtree, DWORD filter )
{
    w->dir = CreateFileW( path, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                          OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL );
    memset( &w->ov, 0, sizeof(w->ov) );
    w->ov.hEvent = CreateEventW( NULL, TRUE, FALSE, NULL );
    ReadDirectoryChangesW( w->dir, w->buffer, sizeof(w->buffer), subtree, filter, NULL, &w->ov, NULL );
}

static void watch_rearm( struct watch *w, BOOL subtree, DWORD filter )
{
    ResetEvent( w->ov.hEvent );
    ReadDirectoryChangesW( w->dir, w->buffer, sizeof(w->buffer), subtree, filter, NULL, &w->ov, NULL );
}

struct record
{
    DWORD action;
    WCHAR name[MAX_PATH];
};

static struct record log_[256];
static int logged;

static int is( const struct record *r, DWORD action, const WCHAR *name )
{
    return r->action == action && !_wcsicmp( r->name, name );
}

/* logs records until one with action for name has arrived (with the rest of its batch) or the timeout runs out;
   returns its index in log_, -1 when it never came */
static int watch_wait( struct watch *w, BOOL subtree, DWORD filter, DWORD action, const WCHAR *name, DWORD ms )
{
    DWORD start = GetTickCount(), size, left;
    int found = -1;

    logged = 0;
    while (found < 0 && (left = GetTickCount() - start) < ms)
    {
        FILE_NOTIFY_INFORMATION *info;

        if (WaitForSingleObject( w->ov.hEvent, ms - left ) != WAIT_OBJECT_0) break;
        if (GetOverlappedResult( w->dir, &w->ov, &size, FALSE ) && size)
        {
            for (info = (FILE_NOTIFY_INFORMATION *)w->buffer;;
                 info = (FILE_NOTIFY_INFORMATION *)((char *)info + info->NextEntryOffset))
            {
                if (logged < ARRAYSIZE(log_))
                {
                    struct record *r = &log_[logged];
                    size_t len = min( info->FileNameLength / sizeof(WCHAR), MAX_PATH - 1 );
                    r->action = info->Action;
                    memcpy( r->name, info->FileName, len * sizeof(WCHAR) );
                    r->name[len] = 0;
                    if (found < 0 && is( r, action, name )) found = logged;
                    logged++;
                }
                if (!info->NextEntryOffset) break;
            }
        }
        watch_rearm( w, subtree, filter );
    }
    return found;
}

static void drain( struct watch *w, BOOL subtree, DWORD filter )
{
    Sleep( 300 );
    watch_wait( w, subtree, filter, 0, L"", 300 );
}

static void watch_stop( struct watch *w )
{
    CancelIo( w->dir );
    CloseHandle( w->ov.hEvent );
    CloseHandle( w->dir );
}

static void path_of( WCHAR *out, const WCHAR *name )
{
    swprintf( out, MAX_PATH, L"%ls\\%ls", root, name );
}

static void write_file( const WCHAR *name, const char *text )
{
    WCHAR path[MAX_PATH];
    HANDLE file;
    DWORD done;

    path_of( path, name );
    file = CreateFileW( path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL );
    WriteFile( file, text, strlen( text ), &done, NULL );
    CloseHandle( file );
}

int wmain( int argc, WCHAR **argv )
{
    const DWORD all = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
                      FILE_NOTIFY_CHANGE_SIZE;
    WCHAR path[MAX_PATH], path2[MAX_PATH], cmd[MAX_PATH * 2];
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    struct watch w, flat;
    HANDLE change;
    int i;

    if (argc == 3 && !wcscmp( argv[1], L"write" ))  /* the second process: a status file, as Epic's helper does */
    {
        wcscpy( root, argv[2] );
        Sleep( 500 );
        write_file( L"status.tx", "ready" );
        return 0;
    }

    GetFullPathNameW( L"watched", MAX_PATH, root, NULL );
    CreateDirectoryW( root, NULL );
    path_of( path, L"sub" );
    CreateDirectoryW( path, NULL );

    watch_start( &w, root, TRUE, all );
    watch_start( &flat, root, FALSE, all );

    swprintf( cmd, MAX_PATH * 2, L"\"%ls\" write \"%ls\"", argv[0], root );
    CreateProcessW( NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi );
    check( watch_wait( &w, TRUE, all, FILE_ACTION_ADDED, L"status.tx", 5000 ) >= 0,
           "another process creates a file: added" );
    WaitForSingleObject( pi.hProcess, 5000 );
    CloseHandle( pi.hProcess );
    CloseHandle( pi.hThread );
    drain( &w, TRUE, all );
    drain( &flat, FALSE, all );

    write_file( L"status.tx", "done" );
    check( watch_wait( &w, TRUE, all, FILE_ACTION_MODIFIED, L"status.tx", 5000 ) >= 0,
           "a file is written again: modified" );
    drain( &w, TRUE, all );
    drain( &flat, FALSE, all );

    path_of( path, L"status.tx" );
    path_of( path2, L"renamed.tx" );
    MoveFileW( path, path2 );
    i = watch_wait( &w, TRUE, all, FILE_ACTION_RENAMED_OLD_NAME, L"status.tx", 5000 );
    check( i >= 0, "a rename: old name" );
    check( i >= 0 && i + 1 < logged && is( &log_[i + 1], FILE_ACTION_RENAMED_NEW_NAME, L"renamed.tx" ),
           "a rename: new name right after the old one" );
    drain( &w, TRUE, all );
    drain( &flat, FALSE, all );

    DeleteFileW( path2 );
    check( watch_wait( &w, TRUE, all, FILE_ACTION_REMOVED, L"renamed.tx", 5000 ) >= 0, "a delete: removed" );
    drain( &w, TRUE, all );
    drain( &flat, FALSE, all );

    write_file( L"sub\\deep.tx", "x" );
    check( watch_wait( &w, TRUE, all, FILE_ACTION_ADDED, L"sub\\deep.tx", 5000 ) >= 0,
           "a file in a subfolder: seen with subtree" );
    check( watch_wait( &flat, FALSE, all, FILE_ACTION_ADDED, L"sub\\deep.tx", 1500 ) < 0,
           "a file in a subfolder: not seen without subtree" );

    watch_stop( &w );
    watch_stop( &flat );

    change = FindFirstChangeNotificationW( root, FALSE, FILE_NOTIFY_CHANGE_FILE_NAME );
    write_file( L"other.tx", "x" );
    check( WaitForSingleObject( change, 5000 ) == WAIT_OBJECT_0, "FindFirstChangeNotification fires" );
    FindCloseChangeNotification( change );

    path_of( path, L"other.tx" );
    DeleteFileW( path );
    path_of( path, L"sub\\deep.tx" );
    DeleteFileW( path );
    path_of( path, L"sub" );
    RemoveDirectoryW( path );
    RemoveDirectoryW( root );

    printf( failures ? "CHANGE PROBE FAILED (%d)\n" : "CHANGE PROBE OK\n", failures );
    return failures != 0;
}

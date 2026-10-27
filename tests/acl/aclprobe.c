/* Permissions given to a folder, read back by another process, the way the Epic Games Launcher prepares the folder
 * a game installs into (tests/acl/run.sh).
 *
 * The launcher starts a second copy of itself as administrator, which grants the players' group full access to the
 * folder (SetEntriesInAcl, SetNamedSecurityInfo). The first copy then reads the folder's permissions
 * (GetNamedSecurityInfo) and looks for that grant; without it the install stops with error DP-07.
 *   aclprobe grant <dir>   creates <dir> and grants BUILTIN\Users full access, inherited by what goes in it
 *   aclprobe check <dir>   reads the permissions of <dir> back, writes a folder and a file into it, then removes all
 * Each check prints PASS or FAIL. */

#include <windows.h>
#include <aclapi.h>
#include <stdio.h>

static int failures;

static void check( int ok, const char *what )
{
    printf( "%s %s\n", ok ? "PASS" : "FAIL", what );
    if (!ok) failures++;
}

/* The access an allow entry for BUILTIN\Users gives, or 0 when there is none */
static DWORD users_access( const WCHAR *path, BYTE *flags )
{
    BYTE users[SECURITY_MAX_SID_SIZE];
    DWORD size = sizeof(users), access = 0, i;
    PSECURITY_DESCRIPTOR sd;
    ACL *dacl;

    CreateWellKnownSid( WinBuiltinUsersSid, NULL, users, &size );
    if (GetNamedSecurityInfoW( path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &dacl, NULL, &sd ))
        return 0;
    for (i = 0; dacl && i < dacl->AceCount; i++)
    {
        ACCESS_ALLOWED_ACE *ace;
        if (!GetAce( dacl, i, (void **)&ace ) || ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
        if (!EqualSid( &ace->SidStart, users )) continue;
        access |= ace->Mask;
        if (flags) *flags = ace->Header.AceFlags;
    }
    LocalFree( sd );
    return access;
}

static int grant( const WCHAR *dir )
{
    BYTE users[SECURITY_MAX_SID_SIZE];
    DWORD size = sizeof(users), err;
    PSECURITY_DESCRIPTOR sd;
    EXPLICIT_ACCESSW ea = {0};
    ACL *old_dacl, *new_dacl;

    check( CreateDirectoryW( dir, NULL ) || GetLastError() == ERROR_ALREADY_EXISTS, "folder created" );
    CreateWellKnownSid( WinBuiltinUsersSid, NULL, users, &size );
    err = GetNamedSecurityInfoW( dir, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &old_dacl, NULL, &sd );
    check( !err, "permissions read" );
    if (err) return 1;
    check( !(users_access( dir, NULL ) & FILE_WRITE_DATA), "players' group can't write yet" );

    ea.grfAccessPermissions = GENERIC_ALL;
    ea.grfAccessMode = GRANT_ACCESS;
    ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName = (WCHAR *)users;
    err = SetEntriesInAclW( 1, &ea, old_dacl, &new_dacl );
    check( !err, "access added" );
    if (err) return 1;
    err = SetNamedSecurityInfoW( (WCHAR *)dir, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, new_dacl, NULL );
    check( !err, "permissions set" );
    check( (users_access( dir, NULL ) & FILE_ALL_ACCESS) == FILE_ALL_ACCESS, "players' group has full access" );
    LocalFree( new_dacl );
    LocalFree( sd );
    return 0;
}

static int check_again( const WCHAR *dir )
{
    WCHAR sub[MAX_PATH], file[MAX_PATH];
    BYTE flags = 0;
    HANDLE h;

    check( (users_access( dir, &flags ) & FILE_ALL_ACCESS) == FILE_ALL_ACCESS,
           "another process sees the players' group's full access" );
    check( (flags & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)) == (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE),
           "the access is marked to be inherited" );

    swprintf( sub, MAX_PATH, L"%ls\\sub", dir );
    swprintf( file, MAX_PATH, L"%ls\\file.txt", dir );
    check( CreateDirectoryW( sub, NULL ), "folder created inside" );
    h = CreateFileW( file, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL );
    check( h != INVALID_HANDLE_VALUE, "file created inside" );
    CloseHandle( h );

    check( DeleteFileW( file ) && RemoveDirectoryW( sub ) && RemoveDirectoryW( dir ), "removed" );
    return 0;
}

int wmain( int argc, WCHAR **argv )
{
    if (argc != 3) return 2;
    if (!wcscmp( argv[1], L"grant" )) grant( argv[2] );
    else
    {
        check_again( argv[2] );
        if (!failures) printf( "ACL PROBE OK\n" );
    }
    return failures != 0;
}

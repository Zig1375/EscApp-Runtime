/* Named CNG keys across runs, the way the Epic Games Launcher uses one (tests/ncrypt/run.sh).
 *
 * The launcher opens its key by name at every start and creates it when there is none; the player's sign-in is
 * bound to it. One run of the probe creates the key and signs with it; a later run, after Wine was stopped, opens
 * it by name and checks it is the same key: same public key, and the first run's signature verifies with it.
 *   keyprobe create <dir>   creates the key, writes its public key and a signature into <dir>
 *   keyprobe check <dir>    opens the key and compares; then deletes it, and checks it is gone
 * Each check prints PASS or FAIL. */

#include <windows.h>
#include <ncrypt.h>
#include <stdio.h>
#include <string.h>

static const WCHAR key_name[] = L"EscApp keyprobe";
static const BYTE hash[32] = {0xe5, 0xca, 0x57, 0x04};
static int failures;

static void check( int ok, const char *what )
{
    printf( "%s %s\n", ok ? "PASS" : "FAIL", what );
    if (!ok) failures++;
}

static int write_file( const char *dir, const char *name, const BYTE *data, DWORD size )
{
    char path[MAX_PATH];
    FILE *f;
    snprintf( path, sizeof(path), "%s\\%s", dir, name );
    if (!(f = fopen( path, "wb" ))) return 0;
    fwrite( data, 1, size, f );
    fclose( f );
    return 1;
}

static DWORD read_file( const char *dir, const char *name, BYTE *data, DWORD size )
{
    char path[MAX_PATH];
    DWORD read;
    FILE *f;
    snprintf( path, sizeof(path), "%s\\%s", dir, name );
    if (!(f = fopen( path, "rb" ))) return 0;
    read = fread( data, 1, size, f );
    fclose( f );
    return read;
}

static int create( NCRYPT_PROV_HANDLE prov, const char *dir )
{
    NCRYPT_KEY_HANDLE key;
    BYTE public[128], signature[128];
    DWORD public_size, signature_size;
    SECURITY_STATUS ret;

    if (NCryptOpenKey( prov, &key, key_name, 0, 0 ) == ERROR_SUCCESS) NCryptDeleteKey( key, 0 );
    ret = NCryptOpenKey( prov, &key, key_name, 0, 0 );
    check( ret == NTE_BAD_KEYSET, "a key that doesn't exist isn't found" );

    ret = NCryptCreatePersistedKey( prov, &key, BCRYPT_ECDSA_P256_ALGORITHM, key_name, 0, 0 );
    check( ret == ERROR_SUCCESS, "named key created" );
    if (ret) return 1;
    check( NCryptFinalizeKey( key, 0 ) == ERROR_SUCCESS, "key finalized" );
    check( NCryptExportKey( key, 0, BCRYPT_ECCPUBLIC_BLOB, NULL, public, sizeof(public), &public_size, 0 )
           == ERROR_SUCCESS, "public key exported" );
    check( NCryptSignHash( key, NULL, (BYTE *)hash, sizeof(hash), signature, sizeof(signature), &signature_size, 0 )
           == ERROR_SUCCESS, "signed" );
    NCryptFreeObject( key );
    check( write_file( dir, "public.bin", public, public_size ) && write_file( dir, "signature.bin", signature,
           signature_size ), "results written" );
    return 0;
}

static int check_again( NCRYPT_PROV_HANDLE prov, const char *dir )
{
    NCRYPT_KEY_HANDLE key;
    BYTE public[128], saved_public[128], signature[128];
    DWORD public_size, saved_public_size, signature_size;
    SECURITY_STATUS ret;

    saved_public_size = read_file( dir, "public.bin", saved_public, sizeof(saved_public) );
    signature_size = read_file( dir, "signature.bin", signature, sizeof(signature) );
    check( saved_public_size && signature_size, "first run's results read" );

    ret = NCryptCreatePersistedKey( prov, &key, BCRYPT_ECDSA_P256_ALGORITHM, key_name, 0, 0 );
    check( ret == NTE_EXISTS, "the name is taken" );

    ret = NCryptOpenKey( prov, &key, key_name, 0, 0 );
    check( ret == ERROR_SUCCESS, "named key opened in a new run" );
    if (ret) return 1;
    check( NCryptExportKey( key, 0, BCRYPT_ECCPUBLIC_BLOB, NULL, public, sizeof(public), &public_size, 0 )
           == ERROR_SUCCESS && public_size == saved_public_size && !memcmp( public, saved_public, public_size ),
           "same public key as the first run" );
    check( NCryptVerifySignature( key, NULL, (BYTE *)hash, sizeof(hash), signature, signature_size, 0 )
           == ERROR_SUCCESS, "first run's signature verifies" );
    check( NCryptSignHash( key, NULL, (BYTE *)hash, sizeof(hash), signature, sizeof(signature), &signature_size, 0 )
           == ERROR_SUCCESS, "signs again" );

    check( NCryptDeleteKey( key, 0 ) == ERROR_SUCCESS, "key deleted" );
    check( NCryptOpenKey( prov, &key, key_name, 0, 0 ) == NTE_BAD_KEYSET, "a deleted key is gone" );
    return 0;
}

int main( int argc, char **argv )
{
    NCRYPT_PROV_HANDLE prov;

    if (argc != 3) return 2;
    if (NCryptOpenStorageProvider( &prov, MS_KEY_STORAGE_PROVIDER, 0 ))
    {
        printf( "FAIL storage provider\n" );
        return 1;
    }
    if (!strcmp( argv[1], "create" )) create( prov, argv[2] );
    else check_again( prov, argv[2] );
    NCryptFreeObject( prov );
    if (!strcmp( argv[1], "check" ) && !failures) printf( "KEY PROBE OK\n" );
    return failures != 0;
}

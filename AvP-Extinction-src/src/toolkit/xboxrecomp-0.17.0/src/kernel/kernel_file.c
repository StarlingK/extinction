#define _GNU_SOURCE
#include "kernel.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <fnmatch.h>
#endif

static const char* get_xbox_path(PXBOX_OBJECT_ATTRIBUTES ObjectAttributes)
{
    if (!ObjectAttributes || !ObjectAttributes->ObjectName ||
        !ObjectAttributes->ObjectName->Buffer)
        return NULL;
    return ObjectAttributes->ObjectName->Buffer;
}

#define XBOX_BYTES_PER_SECTOR       512u
#define XBOX_SECTORS_PER_CLUSTER    32u

uint32_t g_xbox_last_file_error;

uint32_t xbox_LastFileError(void)
{
    return g_xbox_last_file_error;
}

#if defined(_WIN32)

static DWORD xbox_disposition_to_win32(ULONG Disposition)
{
    switch (Disposition) {
        case XBOX_FILE_SUPERSEDE:    return CREATE_ALWAYS;
        case XBOX_FILE_OPEN:         return OPEN_EXISTING;
        case XBOX_FILE_CREATE:       return CREATE_NEW;
        case XBOX_FILE_OPEN_IF:      return OPEN_ALWAYS;
        case XBOX_FILE_OVERWRITE:    return TRUNCATE_EXISTING;
        case XBOX_FILE_OVERWRITE_IF: return CREATE_ALWAYS;
        default:                     return OPEN_EXISTING;
    }
}

static DWORD xbox_access_to_win32(ACCESS_MASK Access)
{
    DWORD result = 0;
    if (Access & XBOX_GENERIC_READ)           result |= GENERIC_READ;
    if (Access & XBOX_GENERIC_WRITE)          result |= GENERIC_WRITE;
    if (Access & XBOX_GENERIC_ALL)            result |= GENERIC_ALL;
    if (Access & XBOX_FILE_READ_DATA)         result |= FILE_READ_DATA;
    if (Access & XBOX_FILE_WRITE_DATA)        result |= FILE_WRITE_DATA;
    if (Access & XBOX_FILE_APPEND_DATA)       result |= FILE_APPEND_DATA;
    if (Access & XBOX_FILE_READ_ATTRIBUTES)   result |= FILE_READ_ATTRIBUTES;
    if (Access & XBOX_FILE_WRITE_ATTRIBUTES)  result |= FILE_WRITE_ATTRIBUTES;
    if (Access & XBOX_SYNCHRONIZE)            result |= SYNCHRONIZE;
    if (Access & XBOX_DELETE)                  result |= DELETE;
    if (result == 0 || result == SYNCHRONIZE)
        result |= GENERIC_READ;
    return result;
}

static DWORD xbox_share_to_win32(ULONG Share)
{
    DWORD result = FILE_SHARE_READ;
    if (Share & 0x01) result |= FILE_SHARE_READ;
    if (Share & 0x02) result |= FILE_SHARE_WRITE;
    if (Share & 0x04) result |= FILE_SHARE_DELETE;
    return result;
}

#define FATX_PENDING_MAX 64
static HANDLE s_overwrite_pending[FATX_PENDING_MAX];
static SRWLOCK s_overwrite_lock = SRWLOCK_INIT;

static int disposition_overwrites(ULONG disposition)
{
    return disposition == XBOX_FILE_SUPERSEDE
        || disposition == XBOX_FILE_OVERWRITE
        || disposition == XBOX_FILE_OVERWRITE_IF;
}

static int overwrite_mark(HANDLE h)
{
    int i, ok = 0;
    AcquireSRWLockExclusive(&s_overwrite_lock);
    for (i = 0; i < FATX_PENDING_MAX; i++)
        if (!s_overwrite_pending[i]) { s_overwrite_pending[i] = h; ok = 1; break; }
    ReleaseSRWLockExclusive(&s_overwrite_lock);
    return ok;
}

static int overwrite_take(HANDLE h)
{
    int i, was = 0;
    if (!h || h == INVALID_HANDLE_VALUE)
        return 0;
    AcquireSRWLockExclusive(&s_overwrite_lock);
    for (i = 0; i < FATX_PENDING_MAX; i++)
        if (s_overwrite_pending[i] == h) { s_overwrite_pending[i] = NULL; was = 1; break; }
    ReleaseSRWLockExclusive(&s_overwrite_lock);
    return was;
}

static void truncate_to_zero(HANDLE h)
{
    LARGE_INTEGER cur, zero;
    zero.QuadPart = 0;
    if (!SetFilePointerEx(h, zero, &cur, FILE_CURRENT))
        cur.QuadPart = 0;
    SetFilePointerEx(h, zero, NULL, FILE_BEGIN);
    if (!SetEndOfFile(h))
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                 "overwrite: truncate failed err=%u", GetLastError());
    SetFilePointerEx(h, cur, NULL, FILE_BEGIN);
}

static void overwrite_settle(HANDLE h)
{
    if (overwrite_take(h))
        truncate_to_zero(h);
}

static ULONG handle_granted_access(HANDLE h)
{
    typedef LONG (WINAPI *query_object_fn)(HANDLE, int, PVOID, ULONG, PULONG);
    static query_object_fn query;
    ULONG info[14];
    if (!query)
        query = (query_object_fn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                                "NtQueryObject");
    if (!query || query(h, 0  , info, sizeof info, NULL) < 0)
        return 0;
    return info[1];
}

static int handle_may_write(HANDLE h)
{
    ULONG granted = handle_granted_access(h);
    return !granted || (granted & (FILE_WRITE_DATA | GENERIC_WRITE | GENERIC_ALL)) != 0;
}

int xbox_host_parent_exists(const xbox_host_char *host_path)
{
    WCHAR dir[MAX_PATH];
    const WCHAR *cut;
    DWORD attrs;

    if (!host_path)
        return 0;
    cut = wcsrchr(host_path, L'\\');
    {
        const WCHAR *fwd = wcsrchr(host_path, L'/');
        if (fwd && (!cut || fwd > cut)) cut = fwd;
    }
    if (!cut || cut == host_path || (size_t)(cut - host_path) >= MAX_PATH)
        return 0;
    memcpy(dir, host_path, (size_t)(cut - host_path) * sizeof(WCHAR));
    dir[cut - host_path] = 0;
    attrs = GetFileAttributesW(dir);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static BOOL translate_obj_path(PXBOX_OBJECT_ATTRIBUTES ObjectAttributes,
                               WCHAR* win_path, DWORD buf_size)
{
    const char* xbox_path = get_xbox_path(ObjectAttributes);
    if (!xbox_path)
        return FALSE;
    if (ObjectAttributes->RootDirectory && xbox_path[0] != '\\' &&
        !(xbox_path[0] && xbox_path[1] == ':')) {

        DWORD used = GetFinalPathNameByHandleW(ObjectAttributes->RootDirectory,
            win_path, buf_size, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (!used || used >= buf_size || used + 1 >= buf_size) return FALSE;
        if (win_path[used - 1] != L'\\') win_path[used++] = L'\\';
        int count = MultiByteToWideChar(CP_ACP, 0, xbox_path, -1,
            win_path + used, (int)(buf_size - used));
        return count != 0;
    }
    return xbox_translate_path(xbox_path, win_path, buf_size);
}

NTSTATUS __stdcall xbox_NtCreateFile(
    PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess,
    ULONG CreateDisposition, ULONG CreateOptions)
{
    WCHAR win_path[MAX_PATH];
    HANDLE h;
    DWORD flags_and_attrs = FILE_ATTRIBUTE_NORMAL;
    (void)AllocationSize;

    if (!FileHandle || !ObjectAttributes)
        return STATUS_INVALID_PARAMETER;

    if (!translate_obj_path(ObjectAttributes, win_path, MAX_PATH)) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_FILE, "NtCreateFile: path translation failed");
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }

    if ((CreateOptions & XBOX_FILE_DIRECTORY_FILE) &&
        GetFileAttributesW(win_path) != INVALID_FILE_ATTRIBUTES &&
        !(GetFileAttributesW(win_path) & FILE_ATTRIBUTE_DIRECTORY)) {
        WCHAR *slash = wcsrchr(win_path, L'\\');
        if (slash && slash != win_path) {
            *slash = 0;
            xbox_log(XBOX_LOG_INFO, XBOX_LOG_FILE,
                     "NtCreateFile: directory open of a device image, "
                     "using its containing directory instead");
        }
    }

    if (!(CreateOptions & (XBOX_FILE_DIRECTORY_FILE | XBOX_FILE_NON_DIRECTORY_FILE))) {
        DWORD existing = GetFileAttributesW(win_path);
        if (existing != INVALID_FILE_ATTRIBUTES && (existing & FILE_ATTRIBUTE_DIRECTORY))
            CreateOptions |= XBOX_FILE_DIRECTORY_FILE;
    }

    if (CreateOptions & XBOX_FILE_DIRECTORY_FILE) {
        if (CreateDisposition == XBOX_FILE_CREATE || CreateDisposition == XBOX_FILE_OPEN_IF)
            CreateDirectoryW(win_path, NULL);
        h = CreateFileW(win_path, xbox_access_to_win32(DesiredAccess),
            xbox_share_to_win32(ShareAccess), NULL, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, NULL);
    } else {
        if (CreateOptions & XBOX_FILE_NO_INTERMEDIATE_BUFFERING)
            flags_and_attrs |= FILE_FLAG_NO_BUFFERING;
        if (FileAttributes & XBOX_FILE_ATTRIBUTE_READONLY)
            flags_and_attrs |= FILE_ATTRIBUTE_READONLY;
        DWORD access = xbox_access_to_win32(DesiredAccess);
        WIN32_FILE_ATTRIBUTE_DATA fad;

        int defer = disposition_overwrites(CreateDisposition)
                 && (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA))
                 && GetFileAttributesExW(win_path, GetFileExInfoStandard, &fad)
                 && !(fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                 && (fad.nFileSizeHigh || fad.nFileSizeLow);
        h = INVALID_HANDLE_VALUE;
        if (defer) {
            h = CreateFileW(win_path, access, xbox_share_to_win32(ShareAccess),
                NULL, OPEN_EXISTING, flags_and_attrs, NULL);
            if (h != INVALID_HANDLE_VALUE)
                overwrite_take(h);
            if (h != INVALID_HANDLE_VALUE && !overwrite_mark(h))
                truncate_to_zero(h);
            else if (h != INVALID_HANDLE_VALUE)
                XBOX_TRACE(XBOX_LOG_FILE, "NtCreateFile: overwrite of %S deferred", win_path);
        }
        if (h == INVALID_HANDLE_VALUE) {
            h = CreateFileW(win_path, access,
                xbox_share_to_win32(ShareAccess), NULL,
                xbox_disposition_to_win32(CreateDisposition), flags_and_attrs, NULL);

            if (h != INVALID_HANDLE_VALUE)
                overwrite_take(h);
        }
    }

    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();

        g_xbox_last_file_error = (uint32_t)err;

        xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                 "NtCreateFile FAILED: %S (err=%u)", win_path, err);
        NTSTATUS st;
        switch (err) {
            case ERROR_FILE_NOT_FOUND: st = STATUS_OBJECT_NAME_NOT_FOUND; break;
            case ERROR_PATH_NOT_FOUND: st = STATUS_OBJECT_PATH_NOT_FOUND; break;
            case ERROR_ACCESS_DENIED:  st = STATUS_ACCESS_DENIED; break;
            case ERROR_ALREADY_EXISTS: st = STATUS_OBJECT_NAME_COLLISION; break;
            case ERROR_TOO_MANY_OPEN_FILES: st = STATUS_TOO_MANY_OPENED_FILES; break;
            default:                   st = STATUS_UNSUCCESSFUL; break;
        }
        if (IoStatusBlock) {
            IoStatusBlock->Status = st;
            IoStatusBlock->Information = 0;
        }
        return st;
    }

    *FileHandle = h;
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = (CreateDisposition == XBOX_FILE_CREATE) ? 2 : 1;
    }
    XBOX_TRACE(XBOX_LOG_FILE, "NtCreateFile: %S -> handle=%p", win_path, h);
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtReadFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
    PLARGE_INTEGER ByteOffset)
{
    DWORD bytes_read = 0;
    BOOL result;
    OVERLAPPED ov;
    (void)ApcRoutine; (void)ApcContext;

    if (!IoStatusBlock)
        return STATUS_INVALID_PARAMETER;

    overwrite_settle(FileHandle);
    if (ByteOffset && ByteOffset->QuadPart >= 0) {
        memset(&ov, 0, sizeof(ov));
        ov.Offset = ByteOffset->LowPart;
        ov.OffsetHigh = ByteOffset->HighPart;
        result = ReadFile(FileHandle, Buffer, Length, &bytes_read, &ov);
    } else {
        result = ReadFile(FileHandle, Buffer, Length, &bytes_read, NULL);
    }

    if (result || GetLastError() == ERROR_HANDLE_EOF) {
        IoStatusBlock->Information = bytes_read;
        if (bytes_read == 0 && Length > 0) {
            IoStatusBlock->Status = STATUS_END_OF_FILE;
            return STATUS_END_OF_FILE;
        }
        IoStatusBlock->Status = STATUS_SUCCESS;
        if (Event) SetEvent(Event);
        return STATUS_SUCCESS;
    }

    XBOX_TRACE(XBOX_LOG_FILE, "NtReadFile(handle=%p, len=%u) failed err=%u",
               FileHandle, Length, GetLastError());
    IoStatusBlock->Status = STATUS_UNSUCCESSFUL;
    IoStatusBlock->Information = 0;
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS __stdcall xbox_NtWriteFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
    PLARGE_INTEGER ByteOffset)
{
    DWORD bytes_written = 0;
    BOOL result;
    OVERLAPPED ov;
    (void)ApcRoutine; (void)ApcContext;

    if (!IoStatusBlock)
        return STATUS_INVALID_PARAMETER;

    overwrite_settle(FileHandle);
    if (ByteOffset && ByteOffset->QuadPart >= 0) {
        memset(&ov, 0, sizeof(ov));
        ov.Offset = ByteOffset->LowPart;
        ov.OffsetHigh = ByteOffset->HighPart;
        result = WriteFile(FileHandle, Buffer, Length, &bytes_written, &ov);
    } else {
        result = WriteFile(FileHandle, Buffer, Length, &bytes_written, NULL);
    }

    if (result) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = bytes_written;
        if (Event) SetEvent(Event);
        return STATUS_SUCCESS;
    }

    XBOX_TRACE(XBOX_LOG_FILE, "NtWriteFile(handle=%p, len=%u) failed err=%u",
               FileHandle, Length, GetLastError());
    IoStatusBlock->Status = STATUS_UNSUCCESSFUL;
    IoStatusBlock->Information = 0;
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS __stdcall xbox_NtClose(HANDLE Handle)
{
    XBOX_TRACE(XBOX_LOG_FILE, "NtClose(handle=%p)", Handle);
    if (Handle && Handle != INVALID_HANDLE_VALUE) {
        overwrite_settle(Handle);
        xbox_dir_context_drop(Handle);
        CloseHandle(Handle);
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_HANDLE;
}

NTSTATUS __stdcall xbox_NtDeleteFile(PXBOX_OBJECT_ATTRIBUTES ObjectAttributes)
{
    WCHAR win_path[MAX_PATH];
    if (!translate_obj_path(ObjectAttributes, win_path, MAX_PATH))
        return STATUS_OBJECT_PATH_NOT_FOUND;

    xbox_log(XBOX_LOG_INFO, XBOX_LOG_FILE, "delete %S", win_path);
    if (DeleteFileW(win_path))    return STATUS_SUCCESS;
    if (RemoveDirectoryW(win_path)) return STATUS_SUCCESS;
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

NTSTATUS __stdcall xbox_NtQueryInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FileInformation, ULONG Length, XBOX_FILE_INFORMATION_CLASS FileInformationClass)
{
    (void)Length;
    if (!IoStatusBlock || !FileInformation)
        return STATUS_INVALID_PARAMETER;

    if (FileInformationClass != XboxFilePositionInformation)
        overwrite_settle(FileHandle);

    switch (FileInformationClass) {
        case XboxFileBasicInformation: {
            PXBOX_FILE_BASIC_INFORMATION info = (PXBOX_FILE_BASIC_INFORMATION)FileInformation;
            BY_HANDLE_FILE_INFORMATION fi;
            if (!GetFileInformationByHandle(FileHandle, &fi))
                return STATUS_UNSUCCESSFUL;
            info->CreationTime.LowPart    = fi.ftCreationTime.dwLowDateTime;
            info->CreationTime.HighPart   = fi.ftCreationTime.dwHighDateTime;
            info->LastAccessTime.LowPart  = fi.ftLastAccessTime.dwLowDateTime;
            info->LastAccessTime.HighPart = fi.ftLastAccessTime.dwHighDateTime;
            info->LastWriteTime.LowPart   = fi.ftLastWriteTime.dwLowDateTime;
            info->LastWriteTime.HighPart  = fi.ftLastWriteTime.dwHighDateTime;
            info->ChangeTime = info->LastWriteTime;
            info->FileAttributes = fi.dwFileAttributes;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_BASIC_INFORMATION);
            return STATUS_SUCCESS;
        }
        case XboxFileStandardInformation: {
            PXBOX_FILE_STANDARD_INFORMATION info = (PXBOX_FILE_STANDARD_INFORMATION)FileInformation;
            BY_HANDLE_FILE_INFORMATION fi;
            if (!GetFileInformationByHandle(FileHandle, &fi))
                return STATUS_UNSUCCESSFUL;
            info->AllocationSize.QuadPart = ((LONGLONG)fi.nFileSizeHigh << 32) | fi.nFileSizeLow;
            info->AllocationSize.QuadPart = (info->AllocationSize.QuadPart + 4095) & ~4095LL;
            info->EndOfFile.QuadPart = ((LONGLONG)fi.nFileSizeHigh << 32) | fi.nFileSizeLow;
            info->NumberOfLinks = fi.nNumberOfLinks;
            info->DeletePending = FALSE;
            info->Directory = (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? TRUE : FALSE;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_STANDARD_INFORMATION);
            return STATUS_SUCCESS;
        }
        case XboxFilePositionInformation: {
            PXBOX_FILE_POSITION_INFORMATION info = (PXBOX_FILE_POSITION_INFORMATION)FileInformation;
            LARGE_INTEGER pos, zero;
            zero.QuadPart = 0;
            if (!SetFilePointerEx(FileHandle, zero, &pos, FILE_CURRENT))
                return STATUS_UNSUCCESSFUL;
            info->CurrentByteOffset = pos;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_POSITION_INFORMATION);
            return STATUS_SUCCESS;
        }
        case XboxFileNetworkOpenInformation: {
            PXBOX_FILE_NETWORK_OPEN_INFORMATION info = (PXBOX_FILE_NETWORK_OPEN_INFORMATION)FileInformation;
            BY_HANDLE_FILE_INFORMATION fi;
            if (!GetFileInformationByHandle(FileHandle, &fi))
                return STATUS_UNSUCCESSFUL;
            info->CreationTime.LowPart    = fi.ftCreationTime.dwLowDateTime;
            info->CreationTime.HighPart   = fi.ftCreationTime.dwHighDateTime;
            info->LastAccessTime.LowPart  = fi.ftLastAccessTime.dwLowDateTime;
            info->LastAccessTime.HighPart = fi.ftLastAccessTime.dwHighDateTime;
            info->LastWriteTime.LowPart   = fi.ftLastWriteTime.dwLowDateTime;
            info->LastWriteTime.HighPart  = fi.ftLastWriteTime.dwHighDateTime;
            info->ChangeTime = info->LastWriteTime;
            info->EndOfFile.QuadPart = ((LONGLONG)fi.nFileSizeHigh << 32) | fi.nFileSizeLow;
            info->AllocationSize.QuadPart = (info->EndOfFile.QuadPart + 4095) & ~4095LL;
            info->FileAttributes = fi.dwFileAttributes;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_NETWORK_OPEN_INFORMATION);
            return STATUS_SUCCESS;
        }
        default:
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                "NtQueryInformationFile: unhandled class %d", FileInformationClass);
            return STATUS_NOT_IMPLEMENTED;
    }
}

NTSTATUS __stdcall xbox_NtSetInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FileInformation, ULONG Length, XBOX_FILE_INFORMATION_CLASS FileInformationClass)
{
    (void)Length;
    if (!IoStatusBlock || !FileInformation)
        return STATUS_INVALID_PARAMETER;

    switch (FileInformationClass) {
        case XboxFilePositionInformation: {
            PXBOX_FILE_POSITION_INFORMATION info = (PXBOX_FILE_POSITION_INFORMATION)FileInformation;
            if (!SetFilePointerEx(FileHandle, info->CurrentByteOffset, NULL, FILE_BEGIN)) {
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                    "SetFilePosition failed: offset=%lld err=%u",
                    (long long)info->CurrentByteOffset.QuadPart, GetLastError());
                return STATUS_UNSUCCESSFUL;
            }
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileEndOfFileInformation: {
            PXBOX_FILE_END_OF_FILE_INFORMATION info = (PXBOX_FILE_END_OF_FILE_INFORMATION)FileInformation;
            LARGE_INTEGER cur, zero = {0};
            if (!handle_may_write(FileHandle)) {
                XBOX_TRACE(XBOX_LOG_FILE, "SetEndOfFile on a read-only handle: access denied");
                IoStatusBlock->Status = STATUS_ACCESS_DENIED;
                return STATUS_ACCESS_DENIED;
            }

            overwrite_take(FileHandle);
            SetFilePointerEx(FileHandle, zero, &cur, FILE_CURRENT);
            SetFilePointerEx(FileHandle, info->EndOfFile, NULL, FILE_BEGIN);
            if (!SetEndOfFile(FileHandle)) {
                DWORD err = GetLastError();
                WCHAR p[MAX_PATH];
                if (!GetFinalPathNameByHandleW(FileHandle, p, MAX_PATH, 0))
                    wcscpy_s(p, MAX_PATH, L"?");
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                    "SetEndOfFile failed: size=%lld err=%u access=0x%08X %S",
                    (long long)info->EndOfFile.QuadPart, err,
                    (unsigned)handle_granted_access(FileHandle), p);
                SetFilePointerEx(FileHandle, cur, NULL, FILE_BEGIN);
                return err == ERROR_ACCESS_DENIED ? STATUS_ACCESS_DENIED
                                                  : STATUS_UNSUCCESSFUL;
            }
            if (cur.QuadPart <= info->EndOfFile.QuadPart)
                SetFilePointerEx(FileHandle, cur, NULL, FILE_BEGIN);
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileAllocationInformation: {

            PXBOX_FILE_END_OF_FILE_INFORMATION info =
                (PXBOX_FILE_END_OF_FILE_INFORMATION)FileInformation;
            FILE_ALLOCATION_INFO fai;
            if (!handle_may_write(FileHandle)) {
                IoStatusBlock->Status = STATUS_ACCESS_DENIED;
                return STATUS_ACCESS_DENIED;
            }
            overwrite_settle(FileHandle);
            fai.AllocationSize = info->EndOfFile;
            if (!SetFileInformationByHandle(FileHandle, FileAllocationInfo,
                                            &fai, sizeof(fai))) {
                LARGE_INTEGER cur, zero = {0};
                SetFilePointerEx(FileHandle, zero, &cur, FILE_CURRENT);
                SetFilePointerEx(FileHandle, info->EndOfFile, NULL, FILE_BEGIN);
                SetEndOfFile(FileHandle);
                SetFilePointerEx(FileHandle, cur, NULL, FILE_BEGIN);
            }
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileDispositionInformation: {
            PXBOX_FILE_DISPOSITION_INFORMATION info = (PXBOX_FILE_DISPOSITION_INFORMATION)FileInformation;
            FILE_DISPOSITION_INFO fdi;
            fdi.DeleteFile = info->DeleteFile;
            if (info->DeleteFile) {
                WCHAR p[MAX_PATH];
                if (!GetFinalPathNameByHandleW(FileHandle, p, MAX_PATH, 0))
                    wcscpy_s(p, MAX_PATH, L"(handle)");
                xbox_log(XBOX_LOG_INFO, XBOX_LOG_FILE, "delete %S", p);
            }
            if (!SetFileInformationByHandle(FileHandle, FileDispositionInfo, &fdi, sizeof(fdi)))
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                         "SetFileDispositionInfo failed: err=%u", GetLastError());
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileBasicInformation: {
            PXBOX_FILE_BASIC_INFORMATION info = (PXBOX_FILE_BASIC_INFORMATION)FileInformation;
            FILETIME ct, at, wt;
            ct.dwLowDateTime = info->CreationTime.LowPart;
            ct.dwHighDateTime = info->CreationTime.HighPart;
            at.dwLowDateTime = info->LastAccessTime.LowPart;
            at.dwHighDateTime = info->LastAccessTime.HighPart;
            wt.dwLowDateTime = info->LastWriteTime.LowPart;
            wt.dwHighDateTime = info->LastWriteTime.HighPart;
            SetFileTime(FileHandle, &ct, &at, &wt);
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        default:

            fprintf(stderr, "  [FILE] NtSetInformationFile: unhandled class %d\n",
                    (int)FileInformationClass);
            fflush(stderr);
            return STATUS_NOT_IMPLEMENTED;
    }
}

NTSTATUS __stdcall xbox_NtQueryVolumeInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FsInformation, ULONG Length, XBOX_FS_INFORMATION_CLASS FsInformationClass)
{
    (void)FileHandle; (void)Length;
    if (!IoStatusBlock || !FsInformation)
        return STATUS_INVALID_PARAMETER;

    switch (FsInformationClass) {
        case XboxFileFsSizeInformation: {
            PXBOX_FILE_FS_SIZE_INFORMATION info = (PXBOX_FILE_FS_SIZE_INFORMATION)FsInformation;
            ULARGE_INTEGER free_bytes, total_bytes, total_free;
            if (GetDiskFreeSpaceExW(NULL, &free_bytes, &total_bytes, &total_free)) {
                info->BytesPerSector = XBOX_BYTES_PER_SECTOR;
                info->SectorsPerAllocationUnit = XBOX_SECTORS_PER_CLUSTER;
                ULONGLONG cs = (ULONGLONG)info->BytesPerSector * info->SectorsPerAllocationUnit;
                info->TotalAllocationUnits.QuadPart = total_bytes.QuadPart / cs;
                info->AvailableAllocationUnits.QuadPart = free_bytes.QuadPart / cs;
            } else {
                info->BytesPerSector = XBOX_BYTES_PER_SECTOR;
                info->SectorsPerAllocationUnit = XBOX_SECTORS_PER_CLUSTER;
                info->TotalAllocationUnits.QuadPart = 1048576;
                info->AvailableAllocationUnits.QuadPart = 524288;
            }
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_FS_SIZE_INFORMATION);
            return STATUS_SUCCESS;
        }
        default:
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                "NtQueryVolumeInformationFile: unhandled class %d", FsInformationClass);
            return STATUS_NOT_IMPLEMENTED;
    }
}

NTSTATUS __stdcall xbox_NtFlushBuffersFile(HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock)
{
    FlushFileBuffers(FileHandle);
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = 0;
    }
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtQueryFullAttributesFile(
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes,
    PXBOX_FILE_NETWORK_OPEN_INFORMATION FileInformation)
{
    WCHAR win_path[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fad;

    if (!FileInformation)
        return STATUS_INVALID_PARAMETER;
    if (!translate_obj_path(ObjectAttributes, win_path, MAX_PATH))
        return STATUS_OBJECT_PATH_NOT_FOUND;

    if (!GetFileAttributesExW(win_path, GetFileExInfoStandard, &fad)) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
            return STATUS_OBJECT_NAME_NOT_FOUND;
        return STATUS_UNSUCCESSFUL;
    }

    FileInformation->CreationTime.LowPart    = fad.ftCreationTime.dwLowDateTime;
    FileInformation->CreationTime.HighPart   = fad.ftCreationTime.dwHighDateTime;
    FileInformation->LastAccessTime.LowPart  = fad.ftLastAccessTime.dwLowDateTime;
    FileInformation->LastAccessTime.HighPart = fad.ftLastAccessTime.dwHighDateTime;
    FileInformation->LastWriteTime.LowPart   = fad.ftLastWriteTime.dwLowDateTime;
    FileInformation->LastWriteTime.HighPart  = fad.ftLastWriteTime.dwHighDateTime;
    FileInformation->ChangeTime = FileInformation->LastWriteTime;
    FileInformation->EndOfFile.QuadPart = ((LONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    FileInformation->AllocationSize.QuadPart = (FileInformation->EndOfFile.QuadPart + 4095) & ~4095LL;
    FileInformation->FileAttributes = fad.dwFileAttributes;
    return STATUS_SUCCESS;
}

#define MAX_DIR_CONTEXTS 64
typedef struct {
    HANDLE file_handle;
    HANDLE find_handle;
    BOOL   first_done;
    WIN32_FIND_DATAW find_data;
} DIR_CONTEXT;

static DIR_CONTEXT s_dir_contexts[MAX_DIR_CONTEXTS];
static CRITICAL_SECTION s_dir_cs;
static BOOL s_dir_cs_init = FALSE;

static DIR_CONTEXT* find_or_create_dir_context(HANDLE FileHandle, BOOL create)
{
    if (!s_dir_cs_init) { InitializeCriticalSection(&s_dir_cs); s_dir_cs_init = TRUE; }
    EnterCriticalSection(&s_dir_cs);
    for (int i = 0; i < MAX_DIR_CONTEXTS; i++) {
        if (s_dir_contexts[i].file_handle == FileHandle && s_dir_contexts[i].find_handle != NULL) {
            LeaveCriticalSection(&s_dir_cs);
            return &s_dir_contexts[i];
        }
    }
    if (!create) { LeaveCriticalSection(&s_dir_cs); return NULL; }
    for (int i = 0; i < MAX_DIR_CONTEXTS; i++) {
        if (s_dir_contexts[i].find_handle == NULL) {
            s_dir_contexts[i].file_handle = FileHandle;
            s_dir_contexts[i].first_done = FALSE;
            LeaveCriticalSection(&s_dir_cs);
            return &s_dir_contexts[i];
        }
    }
    LeaveCriticalSection(&s_dir_cs);
    return NULL;
}

void xbox_dir_context_drop(HANDLE FileHandle)
{
    if (!s_dir_cs_init)
        return;
    EnterCriticalSection(&s_dir_cs);
    for (int i = 0; i < MAX_DIR_CONTEXTS; i++) {
        if (s_dir_contexts[i].file_handle == FileHandle) {
            if (s_dir_contexts[i].find_handle &&
                s_dir_contexts[i].find_handle != INVALID_HANDLE_VALUE)
                FindClose(s_dir_contexts[i].find_handle);
            s_dir_contexts[i].find_handle = NULL;
            s_dir_contexts[i].file_handle = NULL;
            s_dir_contexts[i].first_done = FALSE;
        }
    }
    LeaveCriticalSection(&s_dir_cs);
}

NTSTATUS __stdcall xbox_NtQueryDirectoryFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length,
    XBOX_FILE_INFORMATION_CLASS FileInformationClass,
    PXBOX_ANSI_STRING FileName, BOOLEAN RestartScan)
{
    DIR_CONTEXT* ctx;
    PXBOX_FILE_DIRECTORY_INFORMATION entry;
    (void)Event; (void)ApcRoutine; (void)ApcContext;

    if (!IoStatusBlock || !FileInformation)
        return STATUS_INVALID_PARAMETER;
    IoStatusBlock->Information = 0;
    if (FileInformationClass != XboxFileDirectoryInformation) {
        IoStatusBlock->Status = STATUS_INVALID_INFO_CLASS;
        return STATUS_INVALID_INFO_CLASS;
    }

    ctx = find_or_create_dir_context(FileHandle, TRUE);
    if (!ctx)
        return STATUS_INSUFFICIENT_RESOURCES;

    if (RestartScan || !ctx->first_done) {
        if (ctx->find_handle && ctx->find_handle != INVALID_HANDLE_VALUE) {
            FindClose(ctx->find_handle);
            ctx->find_handle = NULL;
        }
        WCHAR search_path[MAX_PATH];
        WCHAR dir_path[MAX_PATH];
        DWORD path_len = GetFinalPathNameByHandleW(FileHandle, dir_path, MAX_PATH,
                                                   FILE_NAME_NORMALIZED);
        if (path_len == 0 || path_len >= MAX_PATH) {
            IoStatusBlock->Status = STATUS_UNSUCCESSFUL;
            return STATUS_UNSUCCESSFUL;
        }
        WCHAR* clean_path = dir_path;
        if (wcsncmp(clean_path, L"\\\\?\\", 4) == 0)
            clean_path += 4;
        if (FileName && FileName->Buffer) {
            WCHAR pattern_wide[MAX_PATH];
            MultiByteToWideChar(CP_ACP, 0, FileName->Buffer, FileName->Length,
                                pattern_wide, MAX_PATH);
            pattern_wide[FileName->Length] = L'\0';
            swprintf_s(search_path, MAX_PATH, L"%s\\%s", clean_path, pattern_wide);
        } else {
            swprintf_s(search_path, MAX_PATH, L"%s\\*", clean_path);
        }
        ctx->find_handle = FindFirstFileW(search_path, &ctx->find_data);
        if (ctx->find_handle == INVALID_HANDLE_VALUE) {
            ctx->find_handle = NULL;
            IoStatusBlock->Status = STATUS_NO_MORE_FILES;
            return STATUS_NO_MORE_FILES;
        }
        ctx->first_done = TRUE;
    } else {
        if (!FindNextFileW(ctx->find_handle, &ctx->find_data)) {
            FindClose(ctx->find_handle);
            ctx->find_handle = NULL;
            ctx->file_handle = NULL;
            IoStatusBlock->Status = STATUS_NO_MORE_FILES;
            return STATUS_NO_MORE_FILES;
        }
    }

    while (!wcscmp(ctx->find_data.cFileName, L".") ||
           !wcscmp(ctx->find_data.cFileName, L"..")) {
        if (!FindNextFileW(ctx->find_handle, &ctx->find_data)) {
            FindClose(ctx->find_handle);
            ctx->find_handle = NULL;
            ctx->file_handle = NULL;
            IoStatusBlock->Status = STATUS_NO_MORE_FILES;
            return STATUS_NO_MORE_FILES;
        }
    }

    entry = (PXBOX_FILE_DIRECTORY_INFORMATION)FileInformation;
    memset(entry, 0, Length);
    char filename_ansi[MAX_PATH];
    int name_len = WideCharToMultiByte(CP_ACP, 0, ctx->find_data.cFileName, -1,
                                       filename_ansi, MAX_PATH, NULL, NULL);
    if (name_len > 0) name_len--;

    entry->NextEntryOffset = 0;
    entry->FileIndex = 0;
    entry->CreationTime.LowPart   = ctx->find_data.ftCreationTime.dwLowDateTime;
    entry->CreationTime.HighPart  = ctx->find_data.ftCreationTime.dwHighDateTime;
    entry->LastAccessTime.LowPart = ctx->find_data.ftLastAccessTime.dwLowDateTime;
    entry->LastAccessTime.HighPart = ctx->find_data.ftLastAccessTime.dwHighDateTime;
    entry->LastWriteTime.LowPart  = ctx->find_data.ftLastWriteTime.dwLowDateTime;
    entry->LastWriteTime.HighPart = ctx->find_data.ftLastWriteTime.dwHighDateTime;
    entry->ChangeTime = entry->LastWriteTime;
    entry->EndOfFile.QuadPart = ((LONGLONG)ctx->find_data.nFileSizeHigh << 32) | ctx->find_data.nFileSizeLow;
    entry->AllocationSize.QuadPart = (entry->EndOfFile.QuadPart + 4095) & ~4095LL;
    entry->FileAttributes = ctx->find_data.dwFileAttributes;
    entry->FileNameLength = name_len;
    {
        ULONG header_size = (ULONG)((ULONG_PTR)&((PXBOX_FILE_DIRECTORY_INFORMATION)0)->FileName);
        if (name_len > 0 && (header_size + name_len) <= Length)
            memcpy(entry->FileName, filename_ansi, name_len);
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = header_size + name_len;
    }
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_RenameFileByHandle(
    HANDLE FileHandle, PXBOX_OBJECT_ATTRIBUTES NewName, BOOLEAN ReplaceIfExists)
{
    WCHAR src[MAX_PATH], dst[MAX_PATH];
    const char *name = get_xbox_path(NewName);
    DWORD n;

    if (!name || !*name)
        return STATUS_INVALID_PARAMETER;
    n = GetFinalPathNameByHandleW(FileHandle, src, MAX_PATH,
                                  FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!n || n >= MAX_PATH)
        return STATUS_INVALID_HANDLE;

    if (!NewName->RootDirectory && !strchr(name, '\\') && !strchr(name, ':')) {
        WCHAR *slash = wcsrchr(src, L'\\');
        size_t dir = slash ? (size_t)(slash - src) + 1 : 0;
        memcpy(dst, src, dir * sizeof(WCHAR));
        if (!MultiByteToWideChar(CP_ACP, 0, name, -1, dst + dir,
                                 (int)(MAX_PATH - dir)))
            return STATUS_OBJECT_NAME_INVALID;
    } else if (!translate_obj_path(NewName, dst, MAX_PATH)) {
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }

    {
        size_t len = wcslen(dst);
        size_t bytes = sizeof(FILE_RENAME_INFO) + len * sizeof(WCHAR);
        FILE_RENAME_INFO *fri = (FILE_RENAME_INFO *)calloc(1, bytes);
        BOOL ok = FALSE;
        if (fri) {
            fri->ReplaceIfExists = ReplaceIfExists ? TRUE : FALSE;
            fri->RootDirectory = NULL;
            fri->FileNameLength = (DWORD)(len * sizeof(WCHAR));
            memcpy(fri->FileName, dst, (len + 1) * sizeof(WCHAR));
            ok = SetFileInformationByHandle(FileHandle, FileRenameInfo, fri,
                                            (DWORD)bytes);
            free(fri);
        }
        if (!ok)
            ok = MoveFileExW(src, dst,
                             ReplaceIfExists ? MOVEFILE_REPLACE_EXISTING : 0);
        if (!ok) {
            DWORD e = GetLastError();
            return e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS
                 ? STATUS_OBJECT_NAME_COLLISION
                 : e == ERROR_ACCESS_DENIED ? STATUS_ACCESS_DENIED
                 : STATUS_UNSUCCESSFUL;
        }
    }
    fprintf(stderr, "  [FILE] rename %S -> %S\n", src, dst);
    fflush(stderr);
    return STATUS_SUCCESS;
}

#else

static int posix_open_flags(ACCESS_MASK access, ULONG disposition)
{
    int wantWrite = (access & (XBOX_GENERIC_WRITE | XBOX_GENERIC_ALL |
                               XBOX_FILE_WRITE_DATA | XBOX_FILE_APPEND_DATA)) != 0;
    int rw = wantWrite ? O_RDWR : O_RDONLY;
    int extra;

    switch (disposition) {
        case XBOX_FILE_SUPERSEDE:    extra = O_CREAT | O_TRUNC; break;
        case XBOX_FILE_OPEN:         extra = 0;                 break;
        case XBOX_FILE_CREATE:       extra = O_CREAT | O_EXCL;  break;
        case XBOX_FILE_OPEN_IF:      extra = O_CREAT;           break;
        case XBOX_FILE_OVERWRITE:    extra = O_TRUNC;           break;
        case XBOX_FILE_OVERWRITE_IF: extra = O_CREAT | O_TRUNC; break;
        default:                     extra = 0;                 break;
    }

    if ((extra & (O_TRUNC | O_CREAT)) && rw == O_RDONLY)
        rw = O_RDWR;
    if (access & XBOX_FILE_APPEND_DATA)
        extra |= O_APPEND;
    return rw | extra;
}

#define FATX_PENDING_MAX 4096
static volatile unsigned char s_overwrite_pending[FATX_PENDING_MAX];

static int disposition_overwrites(ULONG disposition)
{
    return disposition == XBOX_FILE_SUPERSEDE
        || disposition == XBOX_FILE_OVERWRITE
        || disposition == XBOX_FILE_OVERWRITE_IF;
}

static void overwrite_settle(int fd)
{
    if (fd >= 0 && fd < FATX_PENDING_MAX && s_overwrite_pending[fd]) {
        s_overwrite_pending[fd] = 0;
        if (ftruncate(fd, 0) != 0) {   }
    }
}

static int overwrite_take(int fd)
{
    if (fd >= 0 && fd < FATX_PENDING_MAX && s_overwrite_pending[fd]) {
        s_overwrite_pending[fd] = 0;
        return 1;
    }
    return 0;
}

static void unix_to_filetime(time_t sec, long nsec, LARGE_INTEGER* out)
{

    ULONGLONG t = 116444736000000000ULL
                + (ULONGLONG)sec * 10000000ULL
                + (ULONGLONG)nsec / 100ULL;
    out->LowPart  = (DWORD)(t & 0xFFFFFFFFULL);
    out->HighPart = (LONG)(t >> 32);
}

static ULONG mode_to_xbox_attrs(mode_t m)
{
    ULONG a = 0;
    if (S_ISDIR(m))      a |= XBOX_FILE_ATTRIBUTE_DIRECTORY;
    if (!(m & S_IWUSR))  a |= XBOX_FILE_ATTRIBUTE_READONLY;
    if (a == 0)          a = XBOX_FILE_ATTRIBUTE_NORMAL;
    return a;
}

int xbox_host_parent_exists(const xbox_host_char *host_path)
{
    char dir[MAX_PATH];
    const char *cut;
    struct stat st;

    if (!host_path)
        return 0;
    cut = strrchr(host_path, '/');
    if (!cut || cut == host_path || (size_t)(cut - host_path) >= sizeof dir)
        return 0;
    memcpy(dir, host_path, (size_t)(cut - host_path));
    dir[cut - host_path] = '\0';
    return stat(dir, &st) == 0 && S_ISDIR(st.st_mode);
}

static NTSTATUS errno_to_status(int e)
{
    switch (e) {
        case ENOENT:  return STATUS_OBJECT_NAME_NOT_FOUND;
        case ENOTDIR: return STATUS_OBJECT_PATH_NOT_FOUND;
        case EACCES:
        case EPERM:   return STATUS_ACCESS_DENIED;
        case EEXIST:  return STATUS_OBJECT_NAME_COLLISION;
        case ENOMEM:  return STATUS_NO_MEMORY;
        case EMFILE:
        case ENFILE:  return STATUS_TOO_MANY_OPENED_FILES;
        default:      return STATUS_UNSUCCESSFUL;
    }
}

static int posix_obj_path(PXBOX_OBJECT_ATTRIBUTES oa, char* host_path, size_t n)
{
    const char* xbox_path = get_xbox_path(oa);

    if (!xbox_path)
        return 0;
    if (oa->RootDirectory && xbox_path[0] != '\\'
            && !(xbox_path[0] && xbox_path[1] == ':')) {
        const char* base = w32_handle_path(oa->RootDirectory);
        size_t dir;
        char* p;

        if (!base)
            return 0;
        dir = strlen(base);
        while (dir > 1 && base[dir - 1] == '/')
            dir--;
        if (dir + 1 + strlen(xbox_path) >= n)
            return 0;
        memcpy(host_path, base, dir);
        host_path[dir] = '/';
        strcpy(host_path + dir + 1, xbox_path);
        for (p = host_path + dir + 1; *p; p++)
            if (*p == '\\') *p = '/';
        return 1;
    }
    return xbox_translate_path(xbox_path, host_path, (DWORD)n) ? 1 : 0;
}

NTSTATUS __stdcall xbox_NtCreateFile(
    PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess,
    ULONG CreateDisposition, ULONG CreateOptions)
{
    char host_path[MAX_PATH];
    (void)AllocationSize; (void)FileAttributes; (void)ShareAccess;

    if (!FileHandle || !ObjectAttributes)
        return STATUS_INVALID_PARAMETER;

    if (!posix_obj_path(ObjectAttributes, host_path, MAX_PATH)) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_FILE, "NtCreateFile: path translation failed");
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }

    int fd;

    if (CreateOptions & XBOX_FILE_DIRECTORY_FILE) {
        struct stat st;
        if (stat(host_path, &st) == 0 && S_ISREG(st.st_mode)) {
            char *slash = strrchr(host_path, '/');
            if (slash && slash != host_path) {
                *slash = '\0';
                xbox_log(XBOX_LOG_INFO, XBOX_LOG_FILE,
                         "NtCreateFile: directory open of a device image, "
                         "using its containing directory instead");
            }
        }
    }
    if (CreateOptions & XBOX_FILE_DIRECTORY_FILE) {
        if (CreateDisposition == XBOX_FILE_CREATE || CreateDisposition == XBOX_FILE_OPEN_IF)
            mkdir(host_path, 0755);
        fd = open(host_path, O_RDONLY | O_DIRECTORY);
    } else {
        int flags = posix_open_flags(DesiredAccess, CreateDisposition);
        struct stat st;
        int defer = disposition_overwrites(CreateDisposition)
                 && stat(host_path, &st) == 0 && S_ISREG(st.st_mode)
                 && st.st_size > 0;
        if (defer)
            flags &= ~O_TRUNC;
        fd = open(host_path, flags, 0644);
        if (fd >= 0 && fd < FATX_PENDING_MAX)
            s_overwrite_pending[fd] = (unsigned char)defer;
        else if (fd >= 0 && defer && ftruncate(fd, 0) != 0) {

        }
    }

    if (fd < 0) {
        int e = errno;
        NTSTATUS st = errno_to_status(e);

        int no_dir = e == ENOTDIR || (e == ENOENT && !xbox_host_parent_exists(host_path));
        if (no_dir)
            st = STATUS_OBJECT_PATH_NOT_FOUND;

        g_xbox_last_file_error = no_dir ? 3u
                               : e == ENOENT ? 2u
                               : (e == EACCES || e == EPERM) ? 5u
                               : e == EEXIST ? 80u
                               : (e == EMFILE || e == ENFILE) ? 4u : 0u;

        if (e == EMFILE || e == ENFILE) {
            static volatile LONG warned;
            if (InterlockedExchange(&warned, 1) == 0) {
                fprintf(stderr, "  [FILE] out of host file descriptors (%s): "
                        "raise the open-file limit\n", host_path);
                fflush(stderr);
            }
        }
        XBOX_TRACE(XBOX_LOG_FILE, "NtCreateFile FAILED: %s (errno=%d)", host_path, e);
        if (IoStatusBlock) {
            IoStatusBlock->Status = st;
            IoStatusBlock->Information = 0;
        }
        return st;
    }

    *FileHandle = w32_open_handle(fd, host_path);
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = (CreateDisposition == XBOX_FILE_CREATE) ? 2 : 1;
    }
    XBOX_TRACE(XBOX_LOG_FILE, "NtCreateFile: %s -> handle=%p", host_path, *FileHandle);
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtReadFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
    PLARGE_INTEGER ByteOffset)
{
    (void)ApcRoutine; (void)ApcContext;
    if (!IoStatusBlock)
        return STATUS_INVALID_PARAMETER;

    int fd = w32_handle_fd(FileHandle);
    if (fd < 0) {
        IoStatusBlock->Status = STATUS_INVALID_HANDLE;
        return STATUS_INVALID_HANDLE;
    }

    overwrite_settle(fd);
    if (ByteOffset && ByteOffset->QuadPart >= 0)
        lseek(fd, (off_t)ByteOffset->QuadPart, SEEK_SET);

    ssize_t n = read(fd, Buffer, Length);
    if (n < 0) {
        XBOX_TRACE(XBOX_LOG_FILE, "NtReadFile(handle=%p) errno=%d", FileHandle, errno);
        IoStatusBlock->Status = STATUS_UNSUCCESSFUL;
        IoStatusBlock->Information = 0;
        return STATUS_UNSUCCESSFUL;
    }

    IoStatusBlock->Information = (ULONG_PTR)n;
    if (n == 0 && Length > 0) {
        IoStatusBlock->Status = STATUS_END_OF_FILE;
        return STATUS_END_OF_FILE;
    }
    IoStatusBlock->Status = STATUS_SUCCESS;
    if (Event) SetEvent(Event);
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtWriteFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
    PLARGE_INTEGER ByteOffset)
{
    (void)ApcRoutine; (void)ApcContext;
    if (!IoStatusBlock)
        return STATUS_INVALID_PARAMETER;

    int fd = w32_handle_fd(FileHandle);
    if (fd < 0) {
        IoStatusBlock->Status = STATUS_INVALID_HANDLE;
        return STATUS_INVALID_HANDLE;
    }

    overwrite_settle(fd);
    if (ByteOffset && ByteOffset->QuadPart >= 0)
        lseek(fd, (off_t)ByteOffset->QuadPart, SEEK_SET);

    ssize_t n = write(fd, Buffer, Length);
    if (n < 0) {
        XBOX_TRACE(XBOX_LOG_FILE, "NtWriteFile(handle=%p) errno=%d", FileHandle, errno);
        IoStatusBlock->Status = STATUS_UNSUCCESSFUL;
        IoStatusBlock->Information = 0;
        return STATUS_UNSUCCESSFUL;
    }

    IoStatusBlock->Status = STATUS_SUCCESS;
    IoStatusBlock->Information = (ULONG_PTR)n;
    if (Event) SetEvent(Event);
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtClose(HANDLE Handle)
{
    XBOX_TRACE(XBOX_LOG_FILE, "NtClose(handle=%p)", Handle);
    if (Handle && Handle != INVALID_HANDLE_VALUE) {
        overwrite_settle(w32_handle_fd(Handle));

        xbox_dir_context_drop(Handle);
        CloseHandle(Handle);
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_HANDLE;
}

NTSTATUS __stdcall xbox_NtDeleteFile(PXBOX_OBJECT_ATTRIBUTES ObjectAttributes)
{
    char host_path[MAX_PATH];
    if (!posix_obj_path(ObjectAttributes, host_path, MAX_PATH))
        return STATUS_OBJECT_PATH_NOT_FOUND;
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_FILE, "delete %s", host_path);
    if (unlink(host_path) == 0) return STATUS_SUCCESS;
    if (rmdir(host_path)  == 0) return STATUS_SUCCESS;
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

NTSTATUS __stdcall xbox_NtQueryInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FileInformation, ULONG Length, XBOX_FILE_INFORMATION_CLASS FileInformationClass)
{
    (void)Length;
    if (!IoStatusBlock || !FileInformation)
        return STATUS_INVALID_PARAMETER;

    int fd = w32_handle_fd(FileHandle);
    if (fd < 0)
        return STATUS_INVALID_HANDLE;

    struct stat st;
    if (FileInformationClass != XboxFilePositionInformation) {

        overwrite_settle(fd);
        if (fstat(fd, &st) != 0)
            return STATUS_UNSUCCESSFUL;
    }

    switch (FileInformationClass) {
        case XboxFileBasicInformation: {
            PXBOX_FILE_BASIC_INFORMATION info = (PXBOX_FILE_BASIC_INFORMATION)FileInformation;
            unix_to_filetime(st.st_ctime, 0, &info->CreationTime);
            unix_to_filetime(st.st_atime, 0, &info->LastAccessTime);
            unix_to_filetime(st.st_mtime, 0, &info->LastWriteTime);
            info->ChangeTime = info->LastWriteTime;
            info->FileAttributes = mode_to_xbox_attrs(st.st_mode);
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_BASIC_INFORMATION);
            return STATUS_SUCCESS;
        }
        case XboxFileStandardInformation: {
            PXBOX_FILE_STANDARD_INFORMATION info = (PXBOX_FILE_STANDARD_INFORMATION)FileInformation;
            info->EndOfFile.QuadPart = st.st_size;
            info->AllocationSize.QuadPart = (st.st_size + 4095) & ~4095LL;
            info->NumberOfLinks = (ULONG)st.st_nlink;
            info->DeletePending = FALSE;
            info->Directory = S_ISDIR(st.st_mode) ? TRUE : FALSE;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_STANDARD_INFORMATION);
            return STATUS_SUCCESS;
        }
        case XboxFilePositionInformation: {
            PXBOX_FILE_POSITION_INFORMATION info = (PXBOX_FILE_POSITION_INFORMATION)FileInformation;
            off_t pos = lseek(fd, 0, SEEK_CUR);
            if (pos < 0) return STATUS_UNSUCCESSFUL;
            info->CurrentByteOffset.QuadPart = pos;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_POSITION_INFORMATION);
            return STATUS_SUCCESS;
        }
        case XboxFileNetworkOpenInformation: {
            PXBOX_FILE_NETWORK_OPEN_INFORMATION info = (PXBOX_FILE_NETWORK_OPEN_INFORMATION)FileInformation;
            unix_to_filetime(st.st_ctime, 0, &info->CreationTime);
            unix_to_filetime(st.st_atime, 0, &info->LastAccessTime);
            unix_to_filetime(st.st_mtime, 0, &info->LastWriteTime);
            info->ChangeTime = info->LastWriteTime;
            info->EndOfFile.QuadPart = st.st_size;
            info->AllocationSize.QuadPart = (st.st_size + 4095) & ~4095LL;
            info->FileAttributes = mode_to_xbox_attrs(st.st_mode);
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_NETWORK_OPEN_INFORMATION);
            return STATUS_SUCCESS;
        }
        default:
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                "NtQueryInformationFile: unhandled class %d", FileInformationClass);
            return STATUS_NOT_IMPLEMENTED;
    }
}

NTSTATUS __stdcall xbox_NtSetInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FileInformation, ULONG Length, XBOX_FILE_INFORMATION_CLASS FileInformationClass)
{
    (void)Length;
    if (!IoStatusBlock || !FileInformation)
        return STATUS_INVALID_PARAMETER;

    int fd = w32_handle_fd(FileHandle);
    if (fd < 0)
        return STATUS_INVALID_HANDLE;

    switch (FileInformationClass) {
        case XboxFilePositionInformation: {
            PXBOX_FILE_POSITION_INFORMATION info = (PXBOX_FILE_POSITION_INFORMATION)FileInformation;
            if (lseek(fd, (off_t)info->CurrentByteOffset.QuadPart, SEEK_SET) < 0)
                return STATUS_UNSUCCESSFUL;
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileEndOfFileInformation: {
            PXBOX_FILE_END_OF_FILE_INFORMATION info = (PXBOX_FILE_END_OF_FILE_INFORMATION)FileInformation;

            if ((fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDONLY) {
                IoStatusBlock->Status = STATUS_ACCESS_DENIED;
                return STATUS_ACCESS_DENIED;
            }
            overwrite_take(fd);
            if (ftruncate(fd, (off_t)info->EndOfFile.QuadPart) != 0)
                return STATUS_UNSUCCESSFUL;
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileAllocationInformation: {

            PXBOX_FILE_END_OF_FILE_INFORMATION info =
                (PXBOX_FILE_END_OF_FILE_INFORMATION)FileInformation;
            struct stat st;
            if ((fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDONLY) {
                IoStatusBlock->Status = STATUS_ACCESS_DENIED;
                return STATUS_ACCESS_DENIED;
            }
            overwrite_settle(fd);
            if (fstat(fd, &st) == 0 && (off_t)info->EndOfFile.QuadPart < st.st_size
                    && ftruncate(fd, (off_t)info->EndOfFile.QuadPart) != 0)
                return STATUS_UNSUCCESSFUL;
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileDispositionInformation: {
            PXBOX_FILE_DISPOSITION_INFORMATION info = (PXBOX_FILE_DISPOSITION_INFORMATION)FileInformation;

            if (info->DeleteFile) {
                const char* p = w32_handle_path(FileHandle);
                if (p) {
                    xbox_log(XBOX_LOG_INFO, XBOX_LOG_FILE, "delete %s", p);
                    unlink(p);
                }
            }
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileBasicInformation:

            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        default:

            fprintf(stderr, "  [FILE] NtSetInformationFile: unhandled class %d\n",
                    (int)FileInformationClass);
            fflush(stderr);
            return STATUS_NOT_IMPLEMENTED;
    }
}

NTSTATUS __stdcall xbox_RenameFileByHandle(
    HANDLE FileHandle, PXBOX_OBJECT_ATTRIBUTES NewName, BOOLEAN ReplaceIfExists)
{
    char dst[MAX_PATH];
    const char *src = w32_handle_path(FileHandle);
    const char *name = get_xbox_path(NewName);
    struct stat st;

    if (!src)
        return STATUS_INVALID_HANDLE;
    if (!name || !*name)
        return STATUS_INVALID_PARAMETER;

    if (NewName->RootDirectory
            || (!strchr(name, '\\') && !strchr(name, ':'))) {
        const char *base = NewName->RootDirectory
                         ? w32_handle_path(NewName->RootDirectory) : src;
        const char *slash;
        size_t dir;
        char *p;

        if (!base)
            return STATUS_INVALID_HANDLE;
        if (NewName->RootDirectory) {
            dir = strlen(base);
        } else {
            slash = strrchr(base, '/');
            dir = slash ? (size_t)(slash - base) : 0;
        }
        if (dir + 1 + strlen(name) >= sizeof dst)
            return STATUS_OBJECT_NAME_INVALID;
        memcpy(dst, base, dir);
        dst[dir] = '/';
        strcpy(dst + dir + 1, name[0] == '\\' ? name + 1 : name);
        for (p = dst + dir + 1; *p; p++)
            if (*p == '\\') *p = '/';
    } else if (!xbox_translate_path(name, dst, MAX_PATH)) {
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }

    if (strcmp(src, dst) != 0) {
        if (!ReplaceIfExists && stat(dst, &st) == 0)
            return STATUS_OBJECT_NAME_COLLISION;
        if (rename(src, dst) != 0) {
            int e = errno;
            fprintf(stderr, "  [FILE] rename %s -> %s FAILED (%s)\n",
                    src, dst, strerror(e));
            fflush(stderr);
            return errno_to_status(e);
        }
    }
    fprintf(stderr, "  [FILE] rename %s -> %s\n", src, dst);
    fflush(stderr);
    w32_handle_set_path(FileHandle, dst);
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtQueryVolumeInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FsInformation, ULONG Length, XBOX_FS_INFORMATION_CLASS FsInformationClass)
{
    (void)Length;
    if (!IoStatusBlock || !FsInformation)
        return STATUS_INVALID_PARAMETER;

    switch (FsInformationClass) {
        case XboxFileFsSizeInformation: {
            PXBOX_FILE_FS_SIZE_INFORMATION info = (PXBOX_FILE_FS_SIZE_INFORMATION)FsInformation;
            struct statvfs vfs;
            int fd = w32_handle_fd(FileHandle);

            info->BytesPerSector = XBOX_BYTES_PER_SECTOR;
            info->SectorsPerAllocationUnit = XBOX_SECTORS_PER_CLUSTER;
            if (fd >= 0 && fstatvfs(fd, &vfs) == 0) {
                ULONGLONG cs = (ULONGLONG)info->BytesPerSector * info->SectorsPerAllocationUnit;
                ULONGLONG total = (ULONGLONG)vfs.f_blocks * vfs.f_frsize;
                ULONGLONG avail = (ULONGLONG)vfs.f_bavail * vfs.f_frsize;
                info->TotalAllocationUnits.QuadPart = total / cs;
                info->AvailableAllocationUnits.QuadPart = avail / cs;
            } else {
                info->TotalAllocationUnits.QuadPart = 1048576;
                info->AvailableAllocationUnits.QuadPart = 524288;
            }
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_FS_SIZE_INFORMATION);
            return STATUS_SUCCESS;
        }
        default:
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                "NtQueryVolumeInformationFile: unhandled class %d", FsInformationClass);
            return STATUS_NOT_IMPLEMENTED;
    }
}

NTSTATUS __stdcall xbox_NtFlushBuffersFile(HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock)
{
    int fd = w32_handle_fd(FileHandle);
    if (fd >= 0) fsync(fd);
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = 0;
    }
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtQueryFullAttributesFile(
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes,
    PXBOX_FILE_NETWORK_OPEN_INFORMATION FileInformation)
{
    char host_path[MAX_PATH];
    struct stat st;

    if (!FileInformation)
        return STATUS_INVALID_PARAMETER;
    if (!posix_obj_path(ObjectAttributes, host_path, MAX_PATH))
        return STATUS_OBJECT_PATH_NOT_FOUND;
    if (stat(host_path, &st) != 0)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    unix_to_filetime(st.st_ctime, 0, &FileInformation->CreationTime);
    unix_to_filetime(st.st_atime, 0, &FileInformation->LastAccessTime);
    unix_to_filetime(st.st_mtime, 0, &FileInformation->LastWriteTime);
    FileInformation->ChangeTime = FileInformation->LastWriteTime;
    FileInformation->EndOfFile.QuadPart = st.st_size;
    FileInformation->AllocationSize.QuadPart = (st.st_size + 4095) & ~4095LL;
    FileInformation->FileAttributes = mode_to_xbox_attrs(st.st_mode);
    return STATUS_SUCCESS;
}

#define MAX_DIR_CONTEXTS 64
typedef struct {
    HANDLE handle;
    DIR*   dir;
    char   pattern[64];
} DIR_CONTEXT;

static DIR_CONTEXT s_dir_contexts[MAX_DIR_CONTEXTS];
static CRITICAL_SECTION s_dir_cs;
static BOOL s_dir_cs_init = FALSE;

void xbox_dir_context_drop(HANDLE FileHandle)
{
    if (!s_dir_cs_init)
        return;
    EnterCriticalSection(&s_dir_cs);
    for (int i = 0; i < MAX_DIR_CONTEXTS; i++) {
        if (s_dir_contexts[i].handle == FileHandle) {
            if (s_dir_contexts[i].dir)
                closedir(s_dir_contexts[i].dir);
            s_dir_contexts[i].dir = NULL;
            s_dir_contexts[i].handle = NULL;
        }
    }
    LeaveCriticalSection(&s_dir_cs);
}

NTSTATUS __stdcall xbox_NtQueryDirectoryFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length,
    XBOX_FILE_INFORMATION_CLASS FileInformationClass,
    PXBOX_ANSI_STRING FileName, BOOLEAN RestartScan)
{
    (void)Event; (void)ApcRoutine; (void)ApcContext;
    if (!IoStatusBlock || !FileInformation)
        return STATUS_INVALID_PARAMETER;
    IoStatusBlock->Information = 0;
    if (FileInformationClass != XboxFileDirectoryInformation) {
        IoStatusBlock->Status = STATUS_INVALID_INFO_CLASS;
        return STATUS_INVALID_INFO_CLASS;
    }

    if (!s_dir_cs_init) { InitializeCriticalSection(&s_dir_cs); s_dir_cs_init = TRUE; }
    EnterCriticalSection(&s_dir_cs);

    DIR_CONTEXT* ctx = NULL;
    for (int i = 0; i < MAX_DIR_CONTEXTS; i++)
        if (s_dir_contexts[i].handle == FileHandle) { ctx = &s_dir_contexts[i]; break; }
    if (!ctx) {
        for (int i = 0; i < MAX_DIR_CONTEXTS; i++)
            if (s_dir_contexts[i].handle == NULL) { ctx = &s_dir_contexts[i]; break; }
        if (!ctx) { LeaveCriticalSection(&s_dir_cs); return STATUS_INSUFFICIENT_RESOURCES; }
        ctx->handle = FileHandle;
        ctx->dir = NULL;
    }

    if (RestartScan || ctx->dir == NULL) {
        if (ctx->dir) { closedir(ctx->dir); ctx->dir = NULL; }
        const char* dpath = w32_handle_path(FileHandle);
        if (!dpath) { LeaveCriticalSection(&s_dir_cs); return STATUS_UNSUCCESSFUL; }
        ctx->dir = opendir(dpath);
        if (!ctx->dir) {
            LeaveCriticalSection(&s_dir_cs);
            IoStatusBlock->Status = STATUS_NO_MORE_FILES;
            return STATUS_NO_MORE_FILES;
        }
        if (FileName && FileName->Buffer && FileName->Length > 0) {
            USHORT n = FileName->Length;
            if (n >= sizeof(ctx->pattern)) n = sizeof(ctx->pattern) - 1;
            memcpy(ctx->pattern, FileName->Buffer, n);
            ctx->pattern[n] = '\0';
        } else {
            strcpy(ctx->pattern, "*");
        }
    }

    struct dirent* de;
    const char* dpath = w32_handle_path(FileHandle);
    struct stat st;
    for (;;) {
        de = readdir(ctx->dir);
        if (!de) {
            closedir(ctx->dir);
            ctx->dir = NULL;
            ctx->handle = NULL;
            LeaveCriticalSection(&s_dir_cs);
            IoStatusBlock->Status = STATUS_NO_MORE_FILES;
            return STATUS_NO_MORE_FILES;
        }
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (fnmatch(ctx->pattern, de->d_name, FNM_CASEFOLD) == 0)
            break;
    }

    char full[MAX_PATH];
    snprintf(full, sizeof(full), "%s/%s", dpath ? dpath : ".", de->d_name);
    if (stat(full, &st) != 0)
        memset(&st, 0, sizeof(st));
    LeaveCriticalSection(&s_dir_cs);

    PXBOX_FILE_DIRECTORY_INFORMATION entry = (PXBOX_FILE_DIRECTORY_INFORMATION)FileInformation;
    memset(entry, 0, Length);

    int name_len = (int)strlen(de->d_name);
    entry->NextEntryOffset = 0;
    entry->FileIndex = 0;
    unix_to_filetime(st.st_ctime, 0, &entry->CreationTime);
    unix_to_filetime(st.st_atime, 0, &entry->LastAccessTime);
    unix_to_filetime(st.st_mtime, 0, &entry->LastWriteTime);
    entry->ChangeTime = entry->LastWriteTime;
    entry->EndOfFile.QuadPart = st.st_size;
    entry->AllocationSize.QuadPart = (st.st_size + 4095) & ~4095LL;
    entry->FileAttributes = mode_to_xbox_attrs(st.st_mode);
    entry->FileNameLength = name_len;

    ULONG header_size = (ULONG)((ULONG_PTR)&((PXBOX_FILE_DIRECTORY_INFORMATION)0)->FileName);
    if (name_len > 0 && (header_size + (ULONG)name_len) <= Length)
        memcpy(entry->FileName, de->d_name, name_len);
    IoStatusBlock->Status = STATUS_SUCCESS;
    IoStatusBlock->Information = header_size + name_len;
    return STATUS_SUCCESS;
}

#endif

NTSTATUS __stdcall xbox_NtOpenFile(
    PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    ULONG ShareAccess, ULONG OpenOptions)
{

    return xbox_NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes,
        IoStatusBlock, NULL, 0, ShareAccess, XBOX_FILE_OPEN, OpenOptions);
}

NTSTATUS __stdcall xbox_IoCreateFile(
    PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess,
    ULONG Disposition, ULONG CreateOptions, ULONG Options)
{
    (void)Options;
    return xbox_NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock,
        AllocationSize, FileAttributes, ShareAccess, Disposition, CreateOptions);
}

NTSTATUS __stdcall xbox_NtFsControlFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, ULONG FsControlCode,
    PVOID InputBuffer, ULONG InputBufferLength,
    PVOID OutputBuffer, ULONG OutputBufferLength)
{
    (void)FileHandle; (void)Event; (void)ApcRoutine; (void)ApcContext;
    (void)InputBuffer; (void)InputBufferLength; (void)OutputBuffer; (void)OutputBufferLength;
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE, "NtFsControlFile(0x%X) - stub", FsControlCode);
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_NOT_IMPLEMENTED;
        IoStatusBlock->Information = 0;
    }
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS __stdcall xbox_NtDeviceIoControlFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, ULONG IoControlCode,
    PVOID InputBuffer, ULONG InputBufferLength,
    PVOID OutputBuffer, ULONG OutputBufferLength)
{
    (void)FileHandle; (void)Event; (void)ApcRoutine; (void)ApcContext;
    (void)InputBuffer; (void)InputBufferLength; (void)OutputBuffer; (void)OutputBufferLength;
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE, "NtDeviceIoControlFile(0x%X) - stub", IoControlCode);
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_NOT_IMPLEMENTED;
        IoStatusBlock->Information = 0;
    }
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS __stdcall xbox_NtOpenSymbolicLinkObject(
    PHANDLE LinkHandle, PXBOX_OBJECT_ATTRIBUTES ObjectAttributes)
{

    if (LinkHandle)
        *LinkHandle = (HANDLE)(ULONG_PTR)0xDEAD0001;
    XBOX_TRACE(XBOX_LOG_FILE, "NtOpenSymbolicLinkObject(%s) - stub",
        get_xbox_path(ObjectAttributes) ? get_xbox_path(ObjectAttributes) : "?");
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtQuerySymbolicLinkObject(
    HANDLE LinkHandle, PXBOX_ANSI_STRING LinkTarget, PULONG ReturnedLength)
{
    (void)LinkHandle;
    const char* target = "\\Device\\CdRom0";
    if (LinkTarget && LinkTarget->Buffer) {
        USHORT len = (USHORT)strlen(target);
        if (len < LinkTarget->MaximumLength) {
            memcpy(LinkTarget->Buffer, target, len + 1);
            LinkTarget->Length = len;
        }
    }
    if (ReturnedLength)
        *ReturnedLength = (ULONG)strlen(target);
    return STATUS_SUCCESS;
}


//  operations.cpp  --------------------------------------------------------------------//

//  Copyright 2002-2009 Beman Dawes
//  Copyright 2001 Dietmar Kuehl

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  See library home page at http://www.boost.org/libs/filesystem

//--------------------------------------------------------------------------------------// 

#include <boost/config.hpp>
#if !defined( BOOST_NO_STD_WSTRING )
// Boost.Filesystem V3 and later requires std::wstring support.
// During the transition to V3, libraries are compiled with both V2 and V3 sources.
// On old compilers that don't support V3 anyhow, we just skip everything so the compile
// will succeed and the library can be built.

// define BOOST_FILESYSTEM_SOURCE so that <boost/filesystem/config.hpp> knows
// the library is being built (possibly exporting rather than importing code)

#define BOOST_FILESYSTEM_SOURCE 

#ifndef BOOST_SYSTEM_NO_DEPRECATED 
# define BOOST_SYSTEM_NO_DEPRECATED
#endif

#ifndef _POSIX_PTHREAD_SEMANTICS
# define _POSIX_PTHREAD_SEMANTICS  // Sun readdir_r()needs this
#endif

#if !(defined(__HP_aCC) && defined(_ILP32) && \
      !defined(_STATVFS_ACPP_PROBLEMS_FIXED))
#define _FILE_OFFSET_BITS 64 // at worst, these defines may have no effect,
#endif
#define __USE_FILE_OFFSET64 // but that is harmless on Windows and on POSIX
      // 64-bit systems or on 32-bit systems which don't have files larger 
      // than can be represented by a traditional POSIX/UNIX off_t type. 
      // OTOH, defining them should kick in 64-bit off_t's (and thus 
      // st_size)on 32-bit systems that provide the Large File
      // Support (LFS)interface, such as Linux, Solaris, and IRIX.
      // The defines are given before any headers are included to
      // ensure that they are available to all included headers.
      // That is required at least on Solaris, and possibly on other
      // systems as well.

#include <boost/filesystem/v3/operations.hpp>
#include <boost/scoped_array.hpp>
#include <boost/detail/workaround.hpp>
#include <cstdlib>  // for malloc, free

#ifdef BOOST_FILEYSTEM_INCLUDE_IOSTREAM
# include <iostream>
#endif

namespace fs = boost::filesystem3;
using boost::filesystem3::path;
using boost::filesystem3::filesystem_error;
using boost::system::error_code;
using boost::system::error_category;
using boost::system::system_category;
using std::string;
using std::wstring;

# ifdef BOOST_POSIX_API

#   include <sys/types.h>
#   if !defined(__APPLE__) && !defined(__OpenBSD__)
#     include <sys/statvfs.h>
#     define BOOST_STATVFS statvfs
#     define BOOST_STATVFS_F_FRSIZE vfs.f_frsize
#   else
#     ifdef __OpenBSD__
#     include <sys/param.h>
#     endif
#     include <sys/mount.h>
#     define BOOST_STATVFS statfs
#     define BOOST_STATVFS_F_FRSIZE static_cast<boost::uintmax_t>(vfs.f_bsize)
#   endif
#   include <dirent.h>
#   include <unistd.h>
#   include <fcntl.h>
#   include <utime.h>
#   include "limits.h"

# else // BOOST_WINDOW_API

#   if (defined(__MINGW32__) || defined(__CYGWIN__)) && !defined(WINVER)
      // Versions of MinGW or Cygwin that support Filesystem V3 support at least WINVER 0x501.
      // See MinGW's windef.h
#     define WINVER 0x501
#   endif
#   include <windows.h>
#   include <winnt.h>
#   if !defined(_WIN32_WINNT)
#     define  _WIN32_WINNT   0x0500
#   endif
#   if defined(__BORLANDC__) || defined(__MWERKS__)
#     if defined(__BORLANDC__)
        using std::time_t;
#     endif
#     include <utime.h>
#   else
#     include <sys/utime.h>
#   endif

//  REPARSE_DATA_BUFFER related definitions are found in ntifs.h, which is part of the 
//  Windows Device Driver Kit. Since that's inconvenient, the definitions are provided
//  here. See http://msdn.microsoft.com/en-us/library/ms791514.aspx

#if !defined(REPARSE_DATA_BUFFER_HEADER_SIZE)  // mingw winnt.h does provide the defs

#define SYMLINK_FLAG_RELATIVE 1

typedef struct _REPARSE_DATA_BUFFER {
  ULONG  ReparseTag;
  USHORT  ReparseDataLength;
  USHORT  Reserved;
  union {
    struct {
      USHORT  SubstituteNameOffset;
      USHORT  SubstituteNameLength;
      USHORT  PrintNameOffset;
      USHORT  PrintNameLength;
      ULONG  Flags;
      WCHAR  PathBuffer[1];
  /*  Example of distinction between substitute and print names:
        mklink /d ldrive c:\
        SubstituteName: c:\\??\
        PrintName: c:\
  */
     } SymbolicLinkReparseBuffer;
    struct {
      USHORT  SubstituteNameOffset;
      USHORT  SubstituteNameLength;
      USHORT  PrintNameOffset;
      USHORT  PrintNameLength;
      WCHAR  PathBuffer[1];
      } MountPointReparseBuffer;
    struct {
      UCHAR  DataBuffer[1];
    } GenericReparseBuffer;
  };
} REPARSE_DATA_BUFFER, *PREPARSE_DATA_BUFFER;

#define REPARSE_DATA_BUFFER_HEADER_SIZE \
  FIELD_OFFSET(REPARSE_DATA_BUFFER, GenericReparseBuffer)

#define MAXIMUM_REPARSE_DATA_BUFFER_SIZE  ( 16 * 1024 )
#endif

# endif

//  BOOST_FILESYSTEM_STATUS_CACHE enables file_status cache in
//  dir_itr_increment. The config tests are placed here because some of the
//  macros being tested come from dirent.h.
//
// TODO: find out what macros indicate dirent::d_type present in more libraries
# if defined(BOOST_WINDOWS_API)\
  || defined(_DIRENT_HAVE_D_TYPE)// defined by GNU C library if d_type present
#   define BOOST_FILESYSTEM_STATUS_CACHE
# endif

#include <sys/stat.h>  // even on Windows some functions use stat()
#include <string>
#include <cstring>
#include <cstdio>      // for remove, rename
#include <cerrno>
#include <cassert>
// #include <iostream>    // for debugging only; comment out when not in use

//  POSIX/Windows macros  ----------------------------------------------------//

//  Portions of the POSIX and Windows API's are very similar, except for name,
//  order of arguments, and meaning of zero/non-zero returns. The macros below
//  abstract away those differences. They follow Windows naming and order of
//  arguments, and return true to indicate no error occurred. [POSIX naming,
//  order of arguments, and meaning of return were followed initially, but
//  found to be less clear and cause more coding errors.]

# if defined(BOOST_POSIX_API)

//  POSIX uses a 0 return to indicate success
#   define BOOST_ERRNO    errno 
#   define BOOST_SET_CURRENT_DIRECTORY(P)(::chdir(P)== 0)
#   define BOOST_CREATE_DIRECTORY(P)(::mkdir(P, S_IRWXU|S_IRWXG|S_IRWXO)== 0)
#   define BOOST_CREATE_HARD_LINK(F,T)(::link(T, F)== 0)
#   define BOOST_CREATE_SYMBOLIC_LINK(F,T,Flag)(::symlink(T, F)== 0)
#   define BOOST_REMOVE_DIRECTORY(P)(::rmdir(P)== 0)
#   define BOOST_DELETE_FILE(P)(::unlink(P)== 0)
#   define BOOST_COPY_DIRECTORY(F,T)(!(::stat(from.c_str(), &from_stat)!= 0\
         || ::mkdir(to.c_str(),from_stat.st_mode)!= 0))
#   define BOOST_COPY_FILE(F,T,FailIfExistsBool)copy_file_api(F, T, FailIfExistsBool)
#   define BOOST_MOVE_FILE(OLD,NEW)(::rename(OLD, NEW)== 0)
#   define BOOST_RESIZE_FILE(P,SZ)(::truncate(P, SZ)== 0)

#   define BOOST_ERROR_NOT_SUPPORTED ENOSYS
#   define BOOST_ERROR_ALREADY_EXISTS EEXIST

# else  // BOOST_WINDOWS_API

//  Windows uses a non-0 return to indicate success
#   define BOOST_ERRNO    ::GetLastError()
#   define BOOST_SET_CURRENT_DIRECTORY(P)(::SetCurrentDirectoryW(P)!= 0)
#   define BOOST_CREATE_DIRECTORY(P)(::CreateDirectoryW(P, 0)!= 0)
#   define BOOST_CREATE_HARD_LINK(F,T)(create_hard_link_api(F, T, 0)!= 0)
#   define BOOST_CREATE_SYMBOLIC_LINK(F,T,Flag)(create_symbolic_link_api(F, T, Flag)!= 0)
#   define BOOST_REMOVE_DIRECTORY(P)(::RemoveDirectoryW(P)!= 0)
#   define BOOST_DELETE_FILE(P)(::DeleteFileW(P)!= 0)
#   define BOOST_COPY_DIRECTORY(F,T)(::CreateDirectoryExW(F, T, 0)!= 0)
#   define BOOST_COPY_FILE(F,T,FailIfExistsBool)(::CopyFileW(F, T, FailIfExistsBool)!= 0)
#   define BOOST_MOVE_FILE(OLD,NEW)(::MoveFileExW(OLD, NEW, MOVEFILE_REPLACE_EXISTING)!= 0)
#   define BOOST_RESIZE_FILE(P,SZ)(resize_file_api(P, SZ)!= 0)
#   define BOOST_READ_SYMLINK(P,T)

#   define BOOST_ERROR_ALREADY_EXISTS ERROR_ALREADY_EXISTS
#   define BOOST_ERROR_NOT_SUPPORTED ERROR_NOT_SUPPORTED

# endif

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                        helpers (all operating systems)                              //
//                                                                                      //
//--------------------------------------------------------------------------------------//

namespace
{

# ifdef BOOST_POSIX_API
  const char dot = '.';
# else
  const wchar_t dot = L'.';
# endif

  boost::filesystem3::directory_iterator end_dir_itr;

  const std::size_t buf_size(128);
  const error_code ok;

  bool error(bool was_error, error_code* ec, const string& message)
  {
    if (!was_error)
    {
      if (ec != 0) ec->clear();
    }
    else  
    { //  error
      if (ec == 0)
        BOOST_FILESYSTEM_THROW(filesystem_error(message,
          error_code(BOOST_ERRNO, system_category())));
      else
        ec->assign(BOOST_ERRNO, system_category());
    }
    return was_error;
  }

  bool error(bool was_error, const path& p, error_code* ec, const string& message)
  {
    if (!was_error)
    {
      if (ec != 0) ec->clear();
    }
    else  
    { //  error
      if (ec == 0)
        BOOST_FILESYSTEM_THROW(filesystem_error(message,
          p, error_code(BOOST_ERRNO, system_category())));
      else
        ec->assign(BOOST_ERRNO, system_category());
    }
    return was_error;
  }

  bool error(bool was_error, const path& p1, const path& p2, error_code* ec,
    const string& message)
  {
    if (!was_error)
    {
      if (ec != 0) ec->clear();
    }
    else  
    { //  error
      if (ec == 0)
        BOOST_FILESYSTEM_THROW(filesystem_error(message,
          p1, p2, error_code(BOOST_ERRNO, system_category())));
      else
        ec->assign(BOOST_ERRNO, system_category());
    }
    return was_error;
  }

  bool error(bool was_error, const error_code& result,
    const path& p, error_code* ec, const string& message)
    //  Overwrites ec if there has already been an error
  {
    if (!was_error)
    {
      if (ec != 0) ec->clear();
    }
    else  
    { //  error
      if (ec == 0)
        BOOST_FILESYSTEM_THROW(filesystem_error(message, p, result));
      else
        *ec = result;
    }
    return was_error;
  }

  bool error(bool was_error, const error_code& result,
    const path& p1, const path& p2, error_code* ec, const string& message)
    //  Overwrites ec if there has already been an error
  {
    if (!was_error)
    {
      if (ec != 0) ec->clear();
    }
    else  
    { //  error
      if (ec == 0)
        BOOST_FILESYSTEM_THROW(filesystem_error(message, p1, p2, result));
      else
        *ec = result;
    }
    return was_error;
  }

  bool is_empty_directory(const path& p)
  {
    return fs::directory_iterator(p)== end_dir_itr;
  }

  bool remove_directory(const path& p) // true if succeeds
    { return BOOST_REMOVE_DIRECTORY(p.c_str()); }
  
  bool remove_file(const path& p) // true if succeeds
    { return BOOST_DELETE_FILE(p.c_str()); }
  
  // called by remove and remove_all_aux
  bool remove_file_or_directory(const path& p, fs::file_status sym_stat, error_code* ec)
    // return true if file removed, false if not removed
  {
    if (sym_stat.type()== fs::file_not_found)
    {
      if (ec != 0) ec->clear();
      return false;
    }

    if (fs::is_directory(sym_stat))
    {
      if (error(!remove_directory(p), p, ec, "boost::filesystem::remove"))
        return false;
    }
    else
    {
      if (error(!remove_file(p), p, ec, "boost::filesystem::remove"))
        return false;
    }
    return true;
  }

  boost::uintmax_t remove_all_aux(const path& p, fs::file_status sym_stat,
    error_code* ec)
  {
    boost::uintmax_t count = 1;

    if (!fs::is_symlink(sym_stat)// don't recurse symbolic links
      && fs::is_directory(sym_stat))
    {
      for (fs::directory_iterator itr(p);
            itr != end_dir_itr; ++itr)
      {
        fs::file_status tmp_sym_stat = fs::symlink_status(itr->path(), *ec);
        if (ec != 0 && ec)
          return count;
        count += remove_all_aux(itr->path(), tmp_sym_stat, ec);
      }
    }
    remove_file_or_directory(p, sym_stat, ec);
    return count;
  }

#ifdef BOOST_POSIX_API

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                            POSIX-specific helpers                                    //
//                                                                                      //
//--------------------------------------------------------------------------------------//

  bool not_found_error(int errval)
  {
    return errno == ENOENT || errno == ENOTDIR;
  }

  bool // true if ok
  copy_file_api(const std::string& from_p,
    const std::string& to_p, bool fail_if_exists)
  {
    const std::size_t buf_sz = 32768;
    boost::scoped_array<char> buf(new char [buf_sz]);
    int infile=-1, outfile=-1;  // -1 means not open

    // bug fixed: code previously did a stat()on the from_file first, but that
    // introduced a gratuitous race condition; the stat()is now done after the open()

    if ((infile = ::open(from_p.c_str(), O_RDONLY))< 0)
      { return false; }

    struct stat from_stat;
    if (::stat(from_p.c_str(), &from_stat)!= 0)
      { return false; }

    int oflag = O_CREAT | O_WRONLY;
    if (fail_if_exists)oflag |= O_EXCL;
    if ((outfile = ::open(to_p.c_str(), oflag, from_stat.st_mode))< 0)
    {
      int open_errno = errno;
      BOOST_ASSERT(infile >= 0);
      ::close(infile);
      errno = open_errno;
      return false;
    }

    ssize_t sz, sz_read=1, sz_write;
    while (sz_read > 0
      && (sz_read = ::read(infile, buf.get(), buf_sz))> 0)
    {
      // Allow for partial writes - see Advanced Unix Programming (2nd Ed.),
      // Marc Rochkind, Addison-Wesley, 2004, page 94
      sz_write = 0;
      do
      {
        if ((sz = ::write(outfile, buf.get() + sz_write,
          sz_read - sz_write))< 0)
        { 
          sz_read = sz; // cause read loop termination
          break;        //  and error to be thrown after closes
        }
        sz_write += sz;
      } while (sz_write < sz_read);
    }

    if (::close(infile)< 0)sz_read = -1;
    if (::close(outfile)< 0)sz_read = -1;

    return sz_read >= 0;
  }

# else

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                            Windows-specific helpers                                  //
//                                                                                      //
//--------------------------------------------------------------------------------------//

  bool not_found_error(int errval)
  {
    return errval == ERROR_FILE_NOT_FOUND
      || errval == ERROR_PATH_NOT_FOUND
      || errval == ERROR_INVALID_NAME  // "tools/jam/src/:sys:stat.h", "//foo"
      || errval == ERROR_INVALID_DRIVE  // USB card reader with no card inserted
      || errval == ERROR_NOT_READY  // CD/DVD drive with no disc inserted
      || errval == ERROR_INVALID_PARAMETER  // ":sys:stat.h"
      || errval == ERROR_BAD_PATHNAME  // "//nosuch" on Win64
      || errval == ERROR_BAD_NETPATH;  // "//nosuch" on Win32
  }

  // these constants come from inspecting some Microsoft sample code
  std::time_t to_time_t(const FILETIME & ft)
  {
    __int64 t = (static_cast<__int64>(ft.dwHighDateTime)<< 32)
      + ft.dwLowDateTime;
#   if !defined(BOOST_MSVC) || BOOST_MSVC > 1300 // > VC++ 7.0
    t -= 116444736000000000LL;
#   else
    t -= 116444736000000000;
#   endif
    t /= 10000000;
    return static_cast<std::time_t>(t);
  }

  void to_FILETIME(std::time_t t, FILETIME & ft)
  {
    __int64 temp = t;
    temp *= 10000000;
#   if !defined(BOOST_MSVC) || BOOST_MSVC > 1300 // > VC++ 7.0
    temp += 116444736000000000LL;
#   else
    temp += 116444736000000000;
#   endif
    ft.dwLowDateTime = static_cast<DWORD>(temp);
    ft.dwHighDateTime = static_cast<DWORD>(temp >> 32);
  }

  // Thanks to Jeremy Maitin-Shepard for much help and for permission to
  // base the equivalent()implementation on portions of his 
  // file-equivalence-win32.cpp experimental code.

  struct handle_wrapper
  {
    HANDLE handle;
    handle_wrapper(HANDLE h)
      : handle(h){}
    ~handle_wrapper()
    {
      if (handle != INVALID_HANDLE_VALUE)
        ::CloseHandle(handle);
    }
  };

  HANDLE create_file_handle(path p, DWORD dwDesiredAccess,
    DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes,
    HANDLE hTemplateFile)
  {
    return ::CreateFileW(p.c_str(), dwDesiredAccess, dwShareMode,
      lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes,
      hTemplateFile);
  }

  inline std::size_t get_full_path_name(
    const path& src, std::size_t len, wchar_t* buf, wchar_t** p)
  {
    return static_cast<std::size_t>(
      ::GetFullPathNameW(src.c_str(), static_cast<DWORD>(len), buf, p));
  }

  BOOL resize_file_api(const wchar_t* p, boost::uintmax_t size)
  {
    HANDLE handle = CreateFileW(p, GENERIC_WRITE, 0, 0, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, 0);
    LARGE_INTEGER sz;
    sz.QuadPart = size;
    return handle != INVALID_HANDLE_VALUE
      && ::SetFilePointerEx(handle, sz, 0, FILE_BEGIN)
      && ::SetEndOfFile(handle)
      && ::CloseHandle(handle);
  }

  //  Windows kernel32.dll functions that may or may not be present
  //  must be accessed through pointers

  typedef BOOL (WINAPI *PtrCreateHardLinkW)(
    /*__in*/       LPCWSTR lpFileName,
    /*__in*/       LPCWSTR lpExistingFileName,
    /*__reserved*/ LPSECURITY_ATTRIBUTES lpSecurityAttributes
   );

  PtrCreateHardLinkW create_hard_link_api = PtrCreateHardLinkW(
    ::GetProcAddress(
      ::GetModuleHandle(TEXT("kernel32.dll")), "CreateHardLinkW"));

  typedef BOOLEAN (WINAPI *PtrCreateSymbolicLinkW)(
    /*__in*/ LPCWSTR lpSymlinkFileName,
    /*__in*/ LPCWSTR lpTargetFileName,
    /*__in*/ DWORD dwFlags
   );

  PtrCreateSymbolicLinkW create_symbolic_link_api = PtrCreateSymbolicLinkW(
    ::GetProcAddress(
      ::GetModuleHandle(TEXT("kernel32.dll")), "CreateSymbolicLinkW"));
#endif

//#ifdef BOOST_WINDOWS_API
//
//
//  inline bool get_free_disk_space(const std::wstring& ph,
//    PULARGE_INTEGER avail, PULARGE_INTEGER total, PULARGE_INTEGER free)
//    { return ::GetDiskFreeSpaceExW(ph.c_str(), avail, total, free)!= 0; }
//
//#endif

} // unnamed namespace

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                operations functions declared in operations.hpp                       //
//                            in alphabetic order                                       //
//                                                                                      //
//--------------------------------------------------------------------------------------//

namespace boost
{
namespace filesystem3
{

  BOOST_FILESYSTEM_DECL
  path absolute(const path& p, const path& base)
  {
//    if ( p.empty() || p.is_absolute() )
//      return p;
//    //  recursively calling absolute is sub-optimal, but is simple
//    path abs_base(base.is_absolute() ? base : absolute(base));
//# ifdef BOOST_WINDOWS_API
//    if (p.has_root_directory())
//      return abs_base.root_name() / p;
//    //  !p.has_root_directory
//    if (p.has_root_name())
//      return p.root_name()
//        / abs_base.root_directory() / abs_base.relative_path() / p.relative_path();
//    //  !p.has_root_name()
//# endif
//    return abs_base / p;

    //  recursively calling absolute is sub-optimal, but is sure and simple
    path abs_base(base.is_absolute() ? base : absolute(base));

    //  store expensive to compute values that are needed multiple times
    path p_root_name (p.root_name());
    path base_root_name (abs_base.root_name());
    path p_root_directory (p.root_directory());

    if (p.empty())
      return abs_base;

    if (!p_root_name.empty())  // p.has_root_name()
    {
      if (p_root_directory.empty())  // !p.has_root_directory()
        return p_root_name / abs_base.root_directory()
        / abs_base.relative_path() / p.relative_path();
      // p is absolute, so fall through to return p at end of block
    }

    else if (!p_root_directory.empty())  // p.has_root_directory()
    {
#     ifdef BOOST_POSIX_API
      // POSIX can have root name it it is a network path
      if (base_root_name.empty())   // !abs_base.has_root_name()
        return p;
#     endif
      return base_root_name / p;
    }

    else
    {
      return abs_base / p;
    }

    return p;  // p.is_absolute() is true
  }

namespace detail
{
  BOOST_FILESYSTEM_DECL bool possible_large_file_size_support()
  {
#   ifdef BOOST_POSIX_API
    struct stat lcl_stat;
    return sizeof(lcl_stat.st_size)> 4;
#   else
    return true;
#   endif
  }

  BOOST_FILESYSTEM_DECL
  void copy(const path& from, const path& to, system::error_code* ec)
  {
    file_status s(symlink_status(from, *ec));
    if (ec != 0 && ec)return;

    if(is_symlink(s))
    {
      copy_symlink(from, to, *ec);
    }
    else if(is_directory(s))
    {
      copy_directory(from, to, *ec);
    }
    else if(is_regular_file(s))
    {
      copy_file(from, to, copy_option::fail_if_exists, *ec);
    }
    else
    {
      if (ec == 0)
        BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::copy",
          from, to, error_code(BOOST_ERROR_NOT_SUPPORTED, system_category())));
      ec->assign(BOOST_ERROR_NOT_SUPPORTED, system_category());
    }
  }

  BOOST_FILESYSTEM_DECL
  void copy_directory(const path& from, const path& to, system::error_code* ec)
  {
#   ifdef BOOST_POSIX_API
    struct stat from_stat;
#   endif
    error(!BOOST_COPY_DIRECTORY(from.c_str(), to.c_str()),
      from, to, ec, "boost::filesystem::copy_directory");
  }

  BOOST_FILESYSTEM_DECL
  void copy_file(const path& from, const path& to,
                  BOOST_SCOPED_ENUM(copy_option)option,
                  error_code* ec)
  {
    error(!BOOST_COPY_FILE(from.c_str(), to.c_str(),
      option == copy_option::fail_if_exists),
        from, to, ec, "boost::filesystem::copy_file");
  }

  BOOST_FILESYSTEM_DECL
  void copy_symlink(const path& from, const path& to, system::error_code* ec)
  {
#   ifdef BOOST_POSIX_API  
    path p(read_symlink(from, ec));
    if (ec != 0 && ec) return;
    create_symlink(p, to, ec);

#   elif _WIN32_WINNT < 0x0600  // SDK earlier than Vista and Server 2008
    error(true, error_code(BOOST_ERROR_NOT_SUPPORTED, system_category()), to, from, ec,
      "boost::filesystem::copy_symlink");

#   else  // modern Windows

    // see if actually supported by Windows runtime dll
    if (error(!create_symbolic_link_api,
        error_code(BOOST_ERROR_NOT_SUPPORTED, system_category()),
        to, from, ec,
        "boost::filesystem3::copy_symlink"))
      return;

	  // preconditions met, so attempt the copy
	  error(!::CopyFileExW(from.c_str(), to.c_str(), 0, 0, 0,
		  COPY_FILE_COPY_SYMLINK | COPY_FILE_FAIL_IF_EXISTS), to, from, ec,
		  "boost::filesystem3::copy_symlink");
#   endif

  }

  BOOST_FILESYSTEM_DECL
  bool create_directories(const path& p, system::error_code* ec)
  {
    if (p.empty() || exists(p))
    {
      if (!p.empty() && !is_directory(p))
      {
        if (ec == 0)
        BOOST_FILESYSTEM_THROW(filesystem_error(
            "boost::filesystem::create_directories", p,
            error_code(system::errc::file_exists, system::generic_category())));
        else ec->assign(system::errc::file_exists, system::generic_category());
      }
      return false;
    }

    // First create branch, by calling ourself recursively
    create_directories(p.parent_path(), ec);
    // Now that parent's path exists, create the directory
    create_directory(p, ec);
    return true;
  }

  BOOST_FILESYSTEM_DECL
  bool create_directory(const path& p, error_code* ec)
  {
    if (BOOST_CREATE_DIRECTORY(p.c_str()))
    {
      if (ec != 0) ec->clear();
      return true;
    }

    //  attempt to create directory failed
    int errval(BOOST_ERRNO);  // save reason for failure
    error_code dummy;
    if (errval == BOOST_ERROR_ALREADY_EXISTS && is_directory(p, dummy))
    {
      if (ec != 0) ec->clear();
      return false;
    }

    //  attempt to create directory failed && it doesn't already exist
    if (ec == 0)
      BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::create_directory",
        p, error_code(errval, system_category())));
    else
      ec->assign(errval, system_category());
    return false;
  }

  BOOST_FILESYSTEM_DECL
  void create_directory_symlink(const path& to, const path& from,
                                 system::error_code* ec)
  {
#   if defined(BOOST_WINDOWS_API) && _WIN32_WINNT < 0x0600  // SDK earlier than Vista and Server 2008

    error(true, error_code(BOOST_ERROR_NOT_SUPPORTED, system_category()), to, from, ec,
      "boost::filesystem::create_directory_symlink");
#   else

#     if defined(BOOST_WINDOWS_API) && _WIN32_WINNT >= 0x0600
        // see if actually supported by Windows runtime dll
        if (error(!create_symbolic_link_api,
            error_code(BOOST_ERROR_NOT_SUPPORTED, system_category()),
            to, from, ec,
            "boost::filesystem::create_directory_symlink"))
          return;
#     endif

    error(!BOOST_CREATE_SYMBOLIC_LINK(from.c_str(), to.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY),
      to, from, ec, "boost::filesystem::create_directory_symlink");
#   endif
  }

  BOOST_FILESYSTEM_DECL
  void create_hard_link(const path& to, const path& from, error_code* ec)
  {

#   if defined(BOOST_WINDOWS_API) && _WIN32_WINNT < 0x0500  // SDK earlier than Win 2K

    error(true, error_code(BOOST_ERROR_NOT_SUPPORTED, system_category()), to, from, ec,
      "boost::filesystem::create_hard_link");
#   else

#     if defined(BOOST_WINDOWS_API) && _WIN32_WINNT >= 0x0500
        // see if actually supported by Windows runtime dll
        if (error(!create_hard_link_api,
            error_code(BOOST_ERROR_NOT_SUPPORTED, system_category()),
            to, from, ec,
            "boost::filesystem::create_hard_link"))
          return;
#     endif

    error(!BOOST_CREATE_HARD_LINK(from.c_str(), to.c_str()), to, from, ec,
      "boost::filesystem::create_hard_link");
#   endif
  }

  BOOST_FILESYSTEM_DECL
  void create_symlink(const path& to, const path& from, error_code* ec)
  {
#   if defined(BOOST_WINDOWS_API) && _WIN32_WINNT < 0x0600  // SDK earlier than Vista and Server 2008
    error(true, error_code(BOOST_ERROR_NOT_SUPPORTED, system_category()), to, from, ec,
      "boost::filesystem::create_directory_symlink");
#   else

#     if defined(BOOST_WINDOWS_API) && _WIN32_WINNT >= 0x0600
        // see if actually supported by Windows runtime dll
        if (error(!create_symbolic_link_api,
            error_code(BOOST_ERROR_NOT_SUPPORTED, system_category()),
            to, from, ec,
            "boost::filesystem::create_directory_symlink"))
          return;
#     endif

    error(!BOOST_CREATE_SYMBOLIC_LINK(from.c_str(), to.c_str(), 0),
      to, from, ec, "boost::filesystem::create_directory_symlink");
#   endif
  }

  BOOST_FILESYSTEM_DECL
  path current_path(error_code* ec)
  {
#   ifdef BOOST_POSIX_API
    path cur;
    for (long path_max = 128;; path_max *=2)// loop 'til buffer large enough
    {
      boost::scoped_array<char>
        buf(new char[static_cast<std::size_t>(path_max)]);
      if (::getcwd(buf.get(), static_cast<std::size_t>(path_max))== 0)
      {
        if (error(errno != ERANGE
      // bug in some versions of the Metrowerks C lib on the Mac: wrong errno set 
#         if defined(__MSL__) && (defined(macintosh) || defined(__APPLE__) || defined(__APPLE_CC__))
          && errno != 0
#         endif
          , ec, "boost::filesystem::current_path"))
        {
          break;
        }
      }
      else
      {
        cur = buf.get();
        if (ec != 0) ec->clear();
        break;
      }
    }
    return cur;

#   else
    DWORD sz;
    if ((sz = ::GetCurrentDirectoryW(0, NULL))== 0)sz = 1;
    boost::scoped_array<path::value_type> buf(new path::value_type[sz]);
    error(::GetCurrentDirectoryW(sz, buf.get())== 0, ec,
      "boost::filesystem::current_path");
    return path(buf.get());
#   endif
  }


  BOOST_FILESYSTEM_DECL
  void current_path(const path& p, system::error_code* ec)
  {
    error(!BOOST_SET_CURRENT_DIRECTORY(p.c_str()),
      p, ec, "boost::filesystem::current_path");
  }

  BOOST_FILESYSTEM_DECL
  bool equivalent(const path& p1, const path& p2, system::error_code* ec)
  {
#   ifdef BOOST_POSIX_API
    struct stat s2;
    int e2(::stat(p2.c_str(), &s2));
    struct stat s1;
    int e1(::stat(p1.c_str(), &s1));

    if (e1 != 0 || e2 != 0)
    {
      // if one is invalid and the other isn't then they aren't equivalent,
      // but if both are invalid then it is an error
      error (e1 != 0 && e2 != 0, p1, p2, ec, "boost::filesystem::equivalent");
      return false;
    }

    // both stats now known to be valid
    return  s1.st_dev == s2.st_dev && s1.st_ino == s2.st_ino
        // According to the POSIX stat specs, "The st_ino and st_dev fields
        // taken together uniquely identify the file within the system."
        // Just to be sure, size and mod time are also checked.
        && s1.st_size == s2.st_size && s1.st_mtime == s2.st_mtime;

#   else  // Windows

    // Note well: Physical location on external media is part of the
    // equivalence criteria. If there are no open handles, physical location
    // can change due to defragmentation or other relocations. Thus handles
    // must be held open until location information for both paths has
    // been retrieved.

    // p2 is done first, so any error reported is for p1
    handle_wrapper h2(
      create_file_handle(
          p2.c_str(),
          0,
          FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
          0,
          OPEN_EXISTING,
          FILE_FLAG_BACKUP_SEMANTICS,
          0));

    handle_wrapper h1(
      create_file_handle(
          p1.c_str(),
          0,
          FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
          0,
          OPEN_EXISTING,
          FILE_FLAG_BACKUP_SEMANTICS,
          0));

    if (h1.handle == INVALID_HANDLE_VALUE
      || h2.handle == INVALID_HANDLE_VALUE)
    {
      // if one is invalid and the other isn't, then they aren't equivalent,
      // but if both are invalid then it is an error
      error(h1.handle == INVALID_HANDLE_VALUE
        && h2.handle == INVALID_HANDLE_VALUE, p1, p2, ec,
          "boost::filesystem::equivalent");
      return false;
    }

    // at this point, both handles are known to be valid

    BY_HANDLE_FILE_INFORMATION info1, info2;

    if (error(!::GetFileInformationByHandle(h1.handle, &info1),
      p1, p2, ec, "boost::filesystem::equivalent"))
        return  false;

    if (error(!::GetFileInformationByHandle(h2.handle, &info2),
      p1, p2, ec, "boost::filesystem::equivalent"))
        return  false;

    // In theory, volume serial numbers are sufficient to distinguish between
    // devices, but in practice VSN's are sometimes duplicated, so last write
    // time and file size are also checked.
      return 
        info1.dwVolumeSerialNumber == info2.dwVolumeSerialNumber
        && info1.nFileIndexHigh == info2.nFileIndexHigh
        && info1.nFileIndexLow == info2.nFileIndexLow
        && info1.nFileSizeHigh == info2.nFileSizeHigh
        && info1.nFileSizeLow == info2.nFileSizeLow
        && info1.ftLastWriteTime.dwLowDateTime
          == info2.ftLastWriteTime.dwLowDateTime
        && info1.ftLastWriteTime.dwHighDateTime
          == info2.ftLastWriteTime.dwHighDateTime;

#   endif
  }

  BOOST_FILESYSTEM_DECL
  boost::uintmax_t file_size(const path& p, error_code* ec)
  {
#   ifdef BOOST_POSIX_API

    struct stat path_stat;
    if (error(::stat(p.c_str(), &path_stat)!= 0,
        p, ec, "boost::filesystem::file_size"))
      return static_cast<boost::uintmax_t>(-1);
   if (error(!S_ISREG(path_stat.st_mode),
      error_code(EPERM, system_category()),
        p, ec, "boost::filesystem::file_size"))
      return static_cast<boost::uintmax_t>(-1);

    return static_cast<boost::uintmax_t>(path_stat.st_size);

#   else  // Windows

    // assume uintmax_t is 64-bits on all Windows compilers

    WIN32_FILE_ATTRIBUTE_DATA fad;

    if (error(::GetFileAttributesExW(p.c_str(), ::GetFileExInfoStandard, &fad)== 0,
        p, ec, "boost::filesystem::file_size"))
        return static_cast<boost::uintmax_t>(-1);

    if (error((fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)!= 0,
      error_code(ERROR_NOT_SUPPORTED, system_category()),
        p, ec, "boost::filesystem::file_size"))
      return static_cast<boost::uintmax_t>(-1);

    return (static_cast<boost::uintmax_t>(fad.nFileSizeHigh)
              << (sizeof(fad.nFileSizeLow)*8)) + fad.nFileSizeLow;
#   endif
  }

  BOOST_FILESYSTEM_DECL
  boost::uintmax_t hard_link_count(const path& p, system::error_code* ec)
  {
#   ifdef BOOST_POSIX_API

    struct stat path_stat;
    return error(::stat(p.c_str(), &path_stat)!= 0,
                  p, ec, "boost::filesystem::hard_link_count")
           ? 0
           : static_cast<boost::uintmax_t>(path_stat.st_nlink);

#   else // Windows

    // Link count info is only available through GetFileInformationByHandle
    BY_HANDLE_FILE_INFORMATION info;
    handle_wrapper h(
      create_file_handle(p.c_str(), 0,
          FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
          OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0));
    return
      !error(h.handle == INVALID_HANDLE_VALUE,
              p, ec, "boost::filesystem::hard_link_count")
      && !error(::GetFileInformationByHandle(h.handle, &info)== 0,
                 p, ec, "boost::filesystem::hard_link_count")
           ? info.nNumberOfLinks
           : 0;
#   endif
  }

  BOOST_FILESYSTEM_DECL
  path initial_path(error_code* ec)
  {
      static path init_path;
      if (init_path.empty())
        init_path = current_path(ec);
      else if (ec != 0) ec->clear();
      return init_path;
  }

  BOOST_FILESYSTEM_DECL
  bool is_empty(const path& p, system::error_code* ec)
  {
#   ifdef BOOST_POSIX_API

    struct stat path_stat;
    if (error(::stat(p.c_str(), &path_stat)!= 0,
        p, ec, "boost::filesystem::is_empty"))
      return false;        
    return S_ISDIR(path_stat.st_mode)
      ? is_empty_directory(p)
      : path_stat.st_size == 0;
#   else

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (error(::GetFileAttributesExW(p.c_str(), ::GetFileExInfoStandard, &fad)== 0,
      p, ec, "boost::filesystem::is_empty"))
        return false;

    if (ec != 0) ec->clear();
    return 
      (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        ? is_empty_directory(p)
        : (!fad.nFileSizeHigh && !fad.nFileSizeLow);
#   endif
  }

  BOOST_FILESYSTEM_DECL
  std::time_t last_write_time(const path& p, system::error_code* ec)
  {
#   ifdef BOOST_POSIX_API

    struct stat path_stat;
    if (error(::stat(p.c_str(), &path_stat)!= 0,
      p, ec, "boost::filesystem::last_write_time"))
        return std::time_t(-1);
    return path_stat.st_mtime;

#   else

    handle_wrapper hw(
      create_file_handle(p.c_str(), 0,
        FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0));

    if (error(hw.handle == INVALID_HANDLE_VALUE,
      p, ec, "boost::filesystem::last_write_time"))
        return std::time_t(-1);

    FILETIME lwt;

    if (error(::GetFileTime(hw.handle, 0, 0, &lwt)== 0,
      p, ec, "boost::filesystem::last_write_time"))
        return std::time_t(-1);

    return to_time_t(lwt);
#   endif
  }

  BOOST_FILESYSTEM_DECL
  void last_write_time(const path& p, const std::time_t new_time,
                        system::error_code* ec)
  {
#   ifdef BOOST_POSIX_API

    struct stat path_stat;
    if (error(::stat(p.c_str(), &path_stat)!= 0,
      p, ec, "boost::filesystem::last_write_time"))
        return;
    ::utimbuf buf;
    buf.actime = path_stat.st_atime; // utime()updates access time too:-(
    buf.modtime = new_time;
    error(::utime(p.c_str(), &buf)!= 0,
      p, ec, "boost::filesystem::last_write_time");

#   else

    handle_wrapper hw(
      create_file_handle(p.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0));

    if (error(hw.handle == INVALID_HANDLE_VALUE,
      p, ec, "boost::filesystem::last_write_time"))
        return;

    FILETIME lwt;
    to_FILETIME(new_time, lwt);

    error(::SetFileTime(hw.handle, 0, 0, &lwt)== 0,
      p, ec, "boost::filesystem::last_write_time");
#   endif
  }

  BOOST_FILESYSTEM_DECL
  path read_symlink(const path& p, system::error_code* ec)
  {
    path symlink_path;

#   ifdef BOOST_POSIX_API

    for (std::size_t path_max = 64;; path_max *= 2)// loop 'til buffer large enough
    {
      boost::scoped_array<char> buf(new char[path_max]);
      ssize_t result;
      if ((result=::readlink(p.c_str(), buf.get(), path_max))== -1)
      {
        if (ec == 0)
          BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::read_symlink",
            p, error_code(errno, system_category())));
        else ec->assign(errno, system_category());
        break;
      }
      else
      {
        if(result != static_cast<ssize_t>(path_max))
        {
          symlink_path.assign(buf.get(), buf.get() + result);
          if (ec != 0) ec->clear();
          break;
        }
      }
    }

#   elif _WIN32_WINNT < 0x0600  // SDK earlier than Vista and Server 2008
    error(true, error_code(BOOST_ERROR_NOT_SUPPORTED, system_category()), p, ec,
          "boost::filesystem::read_symlink");
#   else  // Vista and Server 2008 SDK, or later

    union info_t
    {
      char buf[REPARSE_DATA_BUFFER_HEADER_SIZE+MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
      REPARSE_DATA_BUFFER rdb;
    } info;

    handle_wrapper h(
      create_file_handle(p.c_str(),GENERIC_READ, 0, 0, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, 0));

    if (error(h.handle == INVALID_HANDLE_VALUE, p, ec, "boost::filesystem::read_symlink"))
      return symlink_path;

    DWORD sz;

    if (!error(::DeviceIoControl(h.handle, FSCTL_GET_REPARSE_POINT,
          0, 0, info.buf, sizeof(info), &sz, 0) == 0, p, ec,
          "boost::filesystem::read_symlink" ))
      symlink_path.assign(
        static_cast<wchar_t*>(info.rdb.SymbolicLinkReparseBuffer.PathBuffer)
        + info.rdb.SymbolicLinkReparseBuffer.PrintNameOffset/sizeof(wchar_t),
        static_cast<wchar_t*>(info.rdb.SymbolicLinkReparseBuffer.PathBuffer)
        + info.rdb.SymbolicLinkReparseBuffer.PrintNameOffset/sizeof(wchar_t)
        + info.rdb.SymbolicLinkReparseBuffer.PrintNameLength/sizeof(wchar_t));
#     endif
    return symlink_path;
  }
  
  BOOST_FILESYSTEM_DECL
  bool remove(const path& p, error_code* ec)
  {
    error_code tmp_ec;
    file_status sym_status = symlink_status(p, tmp_ec);
    if (error(sym_status.type() == status_error, tmp_ec, p, ec,
        "boost::filesystem::remove"))
      return false;

    return remove_file_or_directory(p, sym_status, ec);
  }

  BOOST_FILESYSTEM_DECL
  boost::uintmax_t remove_all(const path& p, error_code* ec)
  {
    error_code tmp_ec;
    file_status sym_status = symlink_status(p, tmp_ec);
    if (error(sym_status.type() == status_error, tmp_ec, p, ec,
      "boost::filesystem::remove_all"))
      return 0;

    return exists(sym_status) // exists() throws nothing
      ? remove_all_aux(p, sym_status, ec)
      : 0;
  }

  BOOST_FILESYSTEM_DECL
  void rename(const path& old_p, const path& new_p, error_code* ec)
  {
    error(!BOOST_MOVE_FILE(old_p.c_str(), new_p.c_str()), old_p, new_p, ec,
      "boost::filesystem::rename");
  }

  BOOST_FILESYSTEM_DECL
  void resize_file(const path& p, uintmax_t size, system::error_code* ec)
  {
    error(!BOOST_RESIZE_FILE(p.c_str(), size), p, ec, "boost::filesystem::resize_file");
  }

  BOOST_FILESYSTEM_DECL
  space_info space(const path& p, error_code* ec)
  {
#   ifdef BOOST_POSIX_API
    struct BOOST_STATVFS vfs;
    space_info info;
    if (!error(::BOOST_STATVFS(p.c_str(), &vfs)!= 0,
      p, ec, "boost::filesystem::space"))
    {
      info.capacity 
        = static_cast<boost::uintmax_t>(vfs.f_blocks)* BOOST_STATVFS_F_FRSIZE;
      info.free 
        = static_cast<boost::uintmax_t>(vfs.f_bfree)* BOOST_STATVFS_F_FRSIZE;
      info.available
        = static_cast<boost::uintmax_t>(vfs.f_bavail)* BOOST_STATVFS_F_FRSIZE;
    }

#   else
    ULARGE_INTEGER avail, total, free;
    space_info info;

    if (!error(::GetDiskFreeSpaceExW(p.c_str(), &avail, &total, &free)== 0,
       p, ec, "boost::filesystem::space"))
    {
      info.capacity
        = (static_cast<boost::uintmax_t>(total.HighPart)<< 32)
          + total.LowPart;
      info.free
        = (static_cast<boost::uintmax_t>(free.HighPart)<< 32)
          + free.LowPart;
      info.available
        = (static_cast<boost::uintmax_t>(avail.HighPart)<< 32)
          + avail.LowPart;
    }

#   endif

    else
    {
      info.capacity = info.free = info.available = 0;
    }
    return info;
  }

# ifndef BOOST_POSIX_API

  file_status process_status_failure(const path& p, error_code* ec)
  {
    int errval(::GetLastError());
    if (ec != 0)                             // always report errval, even though some
      ec->assign(errval, system_category());   // errval values are not status_errors

    if (not_found_error(errval))
    {
      return file_status(file_not_found);
    }
    else if ((errval == ERROR_SHARING_VIOLATION))
    {
      return file_status(type_unknown);
    }
    if (ec == 0)
      BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::status",
        p, error_code(errval, system_category())));
    return file_status(status_error);
  }
# endif

  BOOST_FILESYSTEM_DECL
  file_status status(const path& p, error_code* ec)
  {
#   ifdef BOOST_POSIX_API

    struct stat path_stat;
    if (::stat(p.c_str(), &path_stat)!= 0)
    {
      if (ec != 0)                            // always report errno, even though some
        ec->assign(errno, system_category());   // errno values are not status_errors

      if (not_found_error(errno))
      {
        return fs::file_status(fs::file_not_found);
      }
      if (ec == 0)
        BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::status",
          p, error_code(errno, system_category())));
      return fs::file_status(fs::status_error);
    }
    if (ec != 0) ec->clear();;
    if (S_ISDIR(path_stat.st_mode))
      return fs::file_status(fs::directory_file);
    if (S_ISREG(path_stat.st_mode))
      return fs::file_status(fs::regular_file);
    if (S_ISBLK(path_stat.st_mode))
      return fs::file_status(fs::block_file);
    if (S_ISCHR(path_stat.st_mode))
      return fs::file_status(fs::character_file);
    if (S_ISFIFO(path_stat.st_mode))
      return fs::file_status(fs::fifo_file);
    if (S_ISSOCK(path_stat.st_mode))
      return fs::file_status(fs::socket_file);
    return fs::file_status(fs::type_unknown);

#   else  // Windows

    DWORD attr(::GetFileAttributesW(p.c_str()));
    if (attr == 0xFFFFFFFF)
    {
      return detail::process_status_failure(p, ec);
    }

    //  symlink handling
    if (attr & FILE_ATTRIBUTE_REPARSE_POINT)// aka symlink
    {
      handle_wrapper h(
        create_file_handle(
            p.c_str(),
            0,  // dwDesiredAccess; attributes only
            FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
            0,  // lpSecurityAttributes
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS,
            0)); // hTemplateFile
      if (h.handle == INVALID_HANDLE_VALUE)
      {
        return detail::process_status_failure(p, ec);
      }
    }

    if (ec != 0) ec->clear();
    return (attr & FILE_ATTRIBUTE_DIRECTORY)
      ? file_status(directory_file)
      : file_status(regular_file);

#   endif
  }

  BOOST_FILESYSTEM_DECL
  file_status symlink_status(const path& p, error_code* ec)
  {
#   ifdef BOOST_POSIX_API

    struct stat path_stat;
    if (::lstat(p.c_str(), &path_stat)!= 0)
    {
      if (ec != 0)                            // always report errno, even though some
        ec->assign(errno, system_category());   // errno values are not status_errors

      if (errno == ENOENT || errno == ENOTDIR) // these are not errors
      {
        return fs::file_status(fs::file_not_found);
      }
      if (ec == 0)
        BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::status",
          p, error_code(errno, system_category())));
      return fs::file_status(fs::status_error);
    }
    if (ec != 0) ec->clear();
    if (S_ISREG(path_stat.st_mode))
      return fs::file_status(fs::regular_file);
    if (S_ISDIR(path_stat.st_mode))
      return fs::file_status(fs::directory_file);
    if (S_ISLNK(path_stat.st_mode))
      return fs::file_status(fs::symlink_file);
    if (S_ISBLK(path_stat.st_mode))
      return fs::file_status(fs::block_file);
    if (S_ISCHR(path_stat.st_mode))
      return fs::file_status(fs::character_file);
    if (S_ISFIFO(path_stat.st_mode))
      return fs::file_status(fs::fifo_file);
    if (S_ISSOCK(path_stat.st_mode))
      return fs::file_status(fs::socket_file);
    return fs::file_status(fs::type_unknown);

#   else  // Windows

    DWORD attr(::GetFileAttributesW(p.c_str()));
    if (attr == 0xFFFFFFFF)
    {
      return detail::process_status_failure(p, ec);
    }

    if (ec != 0) ec->clear();

    if (attr & FILE_ATTRIBUTE_REPARSE_POINT)// aka symlink
      return file_status(symlink_file);

    return (attr & FILE_ATTRIBUTE_DIRECTORY)
      ? file_status(directory_file)
      : file_status(regular_file);

#   endif
  }

  BOOST_FILESYSTEM_DECL
  path system_complete(const path& p, system::error_code* ec)
  {
#   ifdef BOOST_POSIX_API
    return (p.empty() || p.is_absolute())
      ? p : current_path()/ p;

#   else
    if (p.empty())
    {
      if (ec != 0) ec->clear();
      return p;
    }
    wchar_t buf[buf_size];
    wchar_t* pfn;
    std::size_t len = get_full_path_name(p, buf_size, buf, &pfn);

    if (error(len == 0, p, ec, "boost::filesystem::system_complete"))
      return path();

    if (len < buf_size)// len does not include null termination character
      return path(&buf[0]);

    boost::scoped_array<wchar_t> big_buf(new wchar_t[len]);

    return error(get_full_path_name(p, len , big_buf.get(), &pfn)== 0,
      p, ec, "boost::filesystem::system_complete")
      ? path()
      : path(big_buf.get());
#   endif
  }

}  // namespace detail

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                                 directory_entry                                      //
//                                                                                      //
//--------------------------------------------------------------------------------------//

  file_status
  directory_entry::m_get_status(system::error_code* ec) const
  {
    if (!status_known(m_status))
    {
      // optimization: if the symlink status is known, and it isn't a symlink,
      // then status and symlink_status are identical so just copy the
      // symlink status to the regular status.
      if (status_known(m_symlink_status)
        && !is_symlink(m_symlink_status))
      { 
        m_status = m_symlink_status;
        if (ec != 0) ec->clear();
      }
      else m_status = detail::status(m_path, ec);
    }
    else if (ec != 0) ec->clear();
    return m_status;
  }

  file_status
  directory_entry::m_get_symlink_status(system::error_code* ec) const
  {
    if (!status_known(m_symlink_status))
      m_symlink_status = detail::symlink_status(m_path, ec);
    else if (ec != 0) ec->clear();
    return m_symlink_status;
  }

//  dispatch directory_entry supplied here rather than in 
//  <boost/filesystem/path_traits.hpp>, thus avoiding header circularity.
//  test cases are in operations_unit_test.cpp

namespace path_traits
{
  void dispatch(const directory_entry & de,
#                ifdef BOOST_WINDOWS_API
                 std::wstring& to,
#                else   
                 std::string& to,
#                endif
                 const codecvt_type &)
  {
    to = de.path().native();
  }

}  // namespace path_traits
} // namespace filesystem3
} // namespace boost

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                               directory_iterator                                     //
//                                                                                      //
//--------------------------------------------------------------------------------------//

namespace
{
# ifdef BOOST_POSIX_API

  error_code path_max(std::size_t & result)
  // this code is based on Stevens and Rago, Advanced Programming in the
  // UNIX envirnment, 2nd Ed., ISBN 0-201-43307-9, page 49
  {
#   ifdef PATH_MAX
    static std::size_t max = PATH_MAX;
#   else
    static std::size_t max = 0;
#   endif
    if (max == 0)
    {
      errno = 0;
      long tmp = ::pathconf("/", _PC_NAME_MAX);
      if (tmp < 0)
      {
        if (errno == 0)// indeterminate
          max = 4096; // guess
        else return error_code(errno, system_category());
      }
      else max = static_cast<std::size_t>(tmp + 1); // relative root
    }
    result = max;
    return ok;
  }

  error_code dir_itr_first(void *& handle, void *& buffer,
    const char* dir, string& target,
    fs::file_status &, fs::file_status &)
  {
    if ((handle = ::opendir(dir))== 0)
      return error_code(errno, system_category());
    target = string(".");  // string was static but caused trouble
                             // when iteration called from dtor, after
                             // static had already been destroyed
    std::size_t path_size (0);  // initialization quiets gcc warning (ticket #3509)
    error_code ec = path_max(path_size);
    if (ec)return ec;
    dirent de;
    buffer = std::malloc((sizeof(dirent) - sizeof(de.d_name))
      +  path_size + 1); // + 1 for "/0"
    return ok;
  }  

  // warning: the only dirent member updated is d_name
  inline int readdir_r_simulator(DIR * dirp, struct dirent * entry,
    struct dirent ** result)// *result set to 0 on end of directory
  {
    errno = 0;

#   if !defined(__CYGWIN__)\
    && defined(_POSIX_THREAD_SAFE_FUNCTIONS)\
    && defined(_SC_THREAD_SAFE_FUNCTIONS)\
    && (_POSIX_THREAD_SAFE_FUNCTIONS+0 >= 0)\
    && (!defined(__hpux) || defined(_REENTRANT)) \
    && (!defined(_AIX) || defined(__THREAD_SAFE))
    if (::sysconf(_SC_THREAD_SAFE_FUNCTIONS)>= 0)
      { return ::readdir_r(dirp, entry, result); }
#   endif

    struct dirent * p;
    *result = 0;
    if ((p = ::readdir(dirp))== 0)
      return errno;
    std::strcpy(entry->d_name, p->d_name);
    *result = entry;
    return 0;
  }

  error_code dir_itr_increment(void *& handle, void *& buffer,
    string& target, fs::file_status & sf, fs::file_status & symlink_sf)
  {
    BOOST_ASSERT(buffer != 0);
    dirent * entry(static_cast<dirent *>(buffer));
    dirent * result;
    int return_code;
    if ((return_code = readdir_r_simulator(static_cast<DIR*>(handle),
      entry, &result))!= 0)return error_code(errno, system_category());
    if (result == 0)
      return fs::detail::dir_itr_close(handle, buffer);
    target = entry->d_name;
#   ifdef BOOST_FILESYSTEM_STATUS_CACHE
    if (entry->d_type == DT_UNKNOWN) // filesystem does not supply d_type value
    {
      sf = symlink_sf = fs::file_status(fs::status_error);
    }
    else  // filesystem supplies d_type value
    {
      if (entry->d_type == DT_DIR)
        sf = symlink_sf = fs::file_status(fs::directory_file);
      else if (entry->d_type == DT_REG)
        sf = symlink_sf = fs::file_status(fs::regular_file);
      else if (entry->d_type == DT_LNK)
      {
        sf = fs::file_status(fs::status_error);
        symlink_sf = fs::file_status(fs::symlink_file);
      }
      else sf = symlink_sf = fs::file_status(fs::status_error);
    }
#   else
    sf = symlink_sf = fs::file_status(fs::status_error);
#    endif
    return ok;
  }

# else // BOOST_WINDOWS_API

  error_code dir_itr_first(void *& handle, const fs::path& dir,
    wstring& target, fs::file_status & sf, fs::file_status & symlink_sf)
  // Note: an empty root directory has no "." or ".." entries, so this
  // causes a ERROR_FILE_NOT_FOUND error which we do not considered an
  // error. It is treated as eof instead.
  {
    // use a form of search Sebastian Martel reports will work with Win98
    wstring dirpath(dir.wstring());
    dirpath += (dirpath.empty()
      || (dirpath[dirpath.size()-1] != L'\\'
        && dirpath[dirpath.size()-1] != L'/'
        && dirpath[dirpath.size()-1] != L':'))? L"\\*" : L"*";

    WIN32_FIND_DATAW data;
    if ((handle = ::FindFirstFileW(dirpath.c_str(), &data))
      == INVALID_HANDLE_VALUE)
    { 
      handle = 0;
      return error_code( (::GetLastError() == ERROR_FILE_NOT_FOUND
                       // Windows Mobile returns ERROR_NO_MORE_FILES; see ticket #3551                                           
                       || ::GetLastError() == ERROR_NO_MORE_FILES) 
        ? 0 : ::GetLastError(), system_category() );
    }
    target = data.cFileName;
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    { sf.type(fs::directory_file); symlink_sf.type(fs::directory_file); }
    else { sf.type(fs::regular_file); symlink_sf.type(fs::regular_file); }
    return error_code();
  }

  error_code  dir_itr_increment(void *& handle, wstring& target,
    fs::file_status & sf, fs::file_status & symlink_sf)
  {
    WIN32_FIND_DATAW data;
    if (::FindNextFileW(handle, &data)== 0)// fails
    {
      int error = ::GetLastError();
      fs::detail::dir_itr_close(handle);
      return error_code(error == ERROR_NO_MORE_FILES ? 0 : error, system_category());
    }
    target = data.cFileName;
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
      { sf.type(fs::directory_file); symlink_sf.type(fs::directory_file); }
    else { sf.type(fs::regular_file); symlink_sf.type(fs::regular_file); }
    return error_code();
  }
#endif

  const error_code not_found_error_code (
#     ifdef BOOST_WINDOWS_API
        ERROR_PATH_NOT_FOUND
#     else
        ENOENT 
#     endif
        , system_category());

}  // unnamed namespace

namespace boost
{
namespace filesystem3
{

namespace detail
{
  //  dir_itr_close is called both from the ~dir_itr_imp()destructor 
  //  and dir_itr_increment()
  BOOST_FILESYSTEM_DECL
  system::error_code dir_itr_close( // never throws
    void *& handle
#   if defined(BOOST_POSIX_API)
    , void *& buffer
#   endif
   )
  {
#   ifdef BOOST_POSIX_API
    std::free(buffer);
    buffer = 0;
    if (handle == 0)return ok;
    DIR * h(static_cast<DIR*>(handle));
    handle = 0;
    return error_code(::closedir(h)== 0 ? 0 : errno, system_category());

#   else
    if (handle != 0)
    {
      ::FindClose(handle);
      handle = 0;
    }
    return ok;

#   endif
  }

  void directory_iterator_construct(directory_iterator& it,
    const path& p, system::error_code* ec)    
  {
    if (error(p.empty(), not_found_error_code, p, ec,
       "boost::filesystem::directory_iterator::construct"))return;

    path::string_type filename;
    file_status file_stat, symlink_file_stat;
    error_code result = dir_itr_first(it.m_imp->handle,
#     if defined(BOOST_POSIX_API)
      it.m_imp->buffer,
#     endif
      p.c_str(), filename, file_stat, symlink_file_stat);

    if (result)
    {
      it.m_imp.reset();
      error(true, result, p,
        ec, "boost::filesystem::directory_iterator::construct");
      return;
    }
    
    if (it.m_imp->handle == 0)it.m_imp.reset(); // eof, so make end iterator
    else // not eof
    {
      it.m_imp->dir_entry.assign(p / filename,
        file_stat, symlink_file_stat);
      if (filename[0] == dot // dot or dot-dot
        && (filename.size()== 1
          || (filename[1] == dot
            && filename.size()== 2)))
        {  it.increment(); }
    }
  }

  void directory_iterator_increment(directory_iterator& it,
    system::error_code* ec)
  {
    BOOST_ASSERT(it.m_imp.get() && "attempt to increment end iterator");
    BOOST_ASSERT(it.m_imp->handle != 0 && "internal program error");
    
    path::string_type filename;
    file_status file_stat, symlink_file_stat;
    system::error_code temp_ec;

    for (;;)
    {
      temp_ec = dir_itr_increment(it.m_imp->handle,
#       if defined(BOOST_POSIX_API)
        it.m_imp->buffer,
#       endif
        filename, file_stat, symlink_file_stat);

      if (temp_ec)
      {
        it.m_imp.reset();
        if (ec == 0)
          BOOST_FILESYSTEM_THROW(
            filesystem_error("boost::filesystem::directory_iterator::operator++",
              it.m_imp->dir_entry.path().parent_path(),
              error_code(BOOST_ERRNO, system_category())));
        ec->assign(BOOST_ERRNO, system_category());
        return;
      }
      else if (ec != 0) ec->clear();

      if (it.m_imp->handle == 0){ it.m_imp.reset(); return; } // eof, make end
      if (!(filename[0] == dot // !(dot or dot-dot)
        && (filename.size()== 1
          || (filename[1] == dot
            && filename.size()== 2))))
      {
        it.m_imp->dir_entry.replace_filename(
          filename, file_stat, symlink_file_stat);
        return;
      }
    }
  }
}  // namespace detail
} // namespace filesystem3
} // namespace boost

#endif  // no wide character support

#pragma once
#include <windows.h>
typedef void *LPITEMIDLIST, *PIDLIST_ABSOLUTE;
typedef const void *LPCITEMIDLIST;
typedef struct { HWND hwndOwner; LPCITEMIDLIST pidlRoot; LPTSTR pszDisplayName; LPCTSTR lpszTitle; UINT ulFlags; void *lpfn; LPARAM lParam; int iImage; } BROWSEINFO;

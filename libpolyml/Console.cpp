/*
    Title:      Poly/ML Console Window.

    Copyright (c) 2000 David C. J. Matthews

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.
    
    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.
    
    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

*/

#ifdef _WIN32_WCE
#include "winceconfig.h"
#include "wincelib.h"
#include <commctrl.h>
#include <commdlg.h>
#else
#include "winconfig.h"
#endif

#ifdef HAVE_STDIO_H
#include <stdio.h>
#endif

#ifdef HAVE_WINDOWS_H
#include <windows.h>
#endif

#ifdef HAVE_TCHAR_H
#include <tchar.h>
#endif

#ifdef HAVE_IO_H
#include <io.h>
#endif

#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif

#ifdef HAVE_ASSERT_H
#include <assert.h>
#define ASSERT(x)   assert(x)
#else
#define ASSERT(x)
#endif

#include "resource.h"

#include "mpoly.h"
#include "PolyControl.h"
#include "diagnostics.h"
#include "mpoly.h"
#include "run_time.h"
#include "sighandler.h"
#include "console.h"
#include "polyexports.h"

/*
This module takes the place of the Windows console which
has a number of problems, apart from not being a pleasant
user interface.  It creates a main window containing an edit
control, which it has to sub-class so that we can receive all
the characters as they are typed.
I've written this in C using the direct Windows calls to make
it fairly independent of the compiler.  It would definitely be
simpler and cleaner written in C++ using MFC.
DCJM 30/5/2000.
*/

HANDLE hMainThread = NULL; // Handle to ML thread.
HWND hMainWindow = NULL; // Main window - exported.
int useConsole;         // Zero if callers should read from stdin.
HINSTANCE hApplicationInstance;     // Application instance (exported)
static HANDLE  hReadFromML; // Handles to pipe from ML thread
static WNDPROC  wpOrigEditProc; // Saved window proc.
static BOOL fAtEnd;         // True if we are at the end of the window
static HWND hEditWnd;       // Edit sub-window
static CHAR *pchInputBuffer; // Buffer to text read.
static int  nBuffLen;       // Length of input buffer.
static int  nNextPosn;      // Position to add input. (<= nBuffLen)
static int  nAvailable;     // Position of "committed" input (<= nNextPosn)
static int  nReadPosn;      // Position of last read (<= nAvailable)
static BOOL fCtrlC;         // TRUE if we have just pressed ctrl-C.
static CRITICAL_SECTION csIOInterlock;
static HANDLE hInputEvent;  // Signalled when input is available.
static HWND hDDEWindow;     // Window to handle DDE requests from ML thread.

static char *lpszServiceName;

static LPTSTR   lpArgs[100]; // Argument list.
static int      nArgs;
#ifdef _WIN32_WCE
static HWND hCmdBar;
#else
static int initDDEControl(char *lpszName);
static void uninitDDEControl(void);
static DWORD dwDDEInstance;
#endif


// Default DDE service name.
#define POLYMLSERVICE   "PolyML"


/* Messages interpreted by the main window thread. */
#define WM_ADDTEXT      WM_APP
#define WM_DDESTART     (WM_APP+1)
#define WM_DDESTOP      (WM_APP+2)
#define WM_DDEEXEC      (WM_APP+3)

/* These functions are called by the I/O routines to test for input and
   to read from the keyboard. */
int isConsoleInput(void)
{
    int nRes;
    EnterCriticalSection(&csIOInterlock);
    nRes = (nAvailable != nReadPosn) || fCtrlC;
    LeaveCriticalSection(&csIOInterlock);
    return nRes;
}

/* Read characters from the input.  Only returns zero on EOF. */
int getConsoleInput(char *buff, int nChars)
{
    int nRes = 0;
    EnterCriticalSection(&csIOInterlock);
    while (nAvailable == nReadPosn || fCtrlC)
    {
        if (fCtrlC)
        {
            // If we had a control-C we treat it much as an interrupted
            // system call in Unix.
            fCtrlC = FALSE;
            if (nAvailable == nReadPosn) ResetEvent(hInputEvent);
            LeaveCriticalSection(&csIOInterlock);
            errno = EINTR;
            return -1;
        }
        ResetEvent(hInputEvent);
        /* Must block until there is input.
           This will only actually happen when called from HandleINT
           since normally we check that input is available first.
           We check for messages while blocking since we may have a
           DDE hidden window around.
        */
        LeaveCriticalSection(&csIOInterlock);
        while (MsgWaitForMultipleObjects(1, &hInputEvent,
                        FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0+1)
        {
            MSG Msg;
            while (PeekMessage(&Msg, NULL, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&Msg);
                DispatchMessage(&Msg);
            }
        }
        EnterCriticalSection(&csIOInterlock);
    }
    // Copy the available characters into the buffer.
    while (nReadPosn != nAvailable && nChars-- > 0)
    {
        char ch;
        ch = pchInputBuffer[nReadPosn];
        if (ch == 4 || ch == 26)
        {
            // EOF character.  We have to return this as
            // a separate buffer of size zero so if we've
            // already returned some characters we leave it till
            // next time.
            if (nRes == 0) if (++nReadPosn == nBuffLen) nReadPosn = 0;
            break;
        }
        buff[nRes++] = ch;
        if (++nReadPosn == nBuffLen) nReadPosn = 0;
    }
    if (nAvailable == nReadPosn) ResetEvent(hInputEvent);
    LeaveCriticalSection(&csIOInterlock);
    return nRes;
}


/* All addition is made at the end of the text so this function is
   called to find out if we're there. */
static void MoveToEnd(void)
{
    if (! fAtEnd)
    {
        // Make sure any text we add goes at the end.
        LRESULT dwEnd = SendMessage(hEditWnd, WM_GETTEXTLENGTH, 0, 0);
        SendMessage(hEditWnd, EM_SETSEL, dwEnd, dwEnd);
        fAtEnd = TRUE;
    }
}

// Remove lines at the beginning until we have enough space.
// If nChars is bigger than the limit we'll delete everything and
// return.  Returns the space removed.
static DWORD CheckForScreenSpace(LRESULT nChars)
{
    DWORD dwRemoved = 0;
    // TODO: We could avoid these calls by remembering this information.
    LRESULT limit = SendMessage(hEditWnd, EM_GETLIMITTEXT, 0, 0);
    LRESULT size = SendMessage(hEditWnd, WM_GETTEXTLENGTH, 0, 0);
    while (nChars+size >= limit)
    {
        int i;
        if (size == 0) return dwRemoved;
        for (i = 0; i < size; i++)
        {
            if (SendMessage(hEditWnd, EM_LINEFROMCHAR, i, 0) != 0)
                break;
        }
        SendMessage(hEditWnd, EM_SETSEL, 0, i);
        SendMessage(hEditWnd, WM_CLEAR, 0, 0);
        fAtEnd = FALSE;
        MoveToEnd();
        size -= i;
        dwRemoved += i;
    }
    return dwRemoved;
}

// Expand the buffer if necessary to allow room for
// additional characters.
static void CheckForBufferSpace(int nChars)
{
    BOOL fOverflow;
    if (nNextPosn >= nReadPosn)
        fOverflow = nNextPosn+nChars >= nReadPosn+nBuffLen;
    else fOverflow = nNextPosn+nChars >= nReadPosn;
    if (fOverflow)
    {
        int nOldLen = nBuffLen;
        // Need more space.
        nBuffLen = nBuffLen + nChars + nBuffLen/2;
        pchInputBuffer = (char*)realloc(pchInputBuffer, nBuffLen);
        // Have to copy any data that has wrapped round to the
        // new area.
        if (nNextPosn < nReadPosn)
        {
            int nExtra = nBuffLen-nOldLen;
            if (nExtra >= nNextPosn)
            {
                // All the space before will fit in the new area.
                memcpy(pchInputBuffer+nOldLen, pchInputBuffer, nNextPosn);
            }
            else
            {
                // Some of the space before will fit but not all.
                memcpy(pchInputBuffer+nOldLen, pchInputBuffer, nExtra);
                memmove(pchInputBuffer, pchInputBuffer+nExtra,
                        nNextPosn-nExtra);
            }
            // Adjust these pointers modulo the old and new lengths.
            if (nAvailable < nNextPosn) nAvailable += nOldLen;
            if (nAvailable >= nBuffLen) nAvailable -= nBuffLen;
            nNextPosn += nOldLen;
            if (nNextPosn >= nBuffLen) nNextPosn -= nBuffLen;
        }
    }
    ASSERT(nBuffLen >= 0 && nAvailable >= 0 && nNextPosn >= 0 &&
           nReadPosn >= 0 &&
           nAvailable < nBuffLen && nReadPosn < nBuffLen &&
           nReadPosn < nBuffLen);
    if (nNextPosn > nReadPosn)
        ASSERT(nAvailable >= nReadPosn && nAvailable <= nNextPosn);
    else ASSERT(nNextPosn != nReadPosn &&
                nAvailable <= nNextPosn || nAvailable >= nReadPosn);
}

#ifndef _WIN32_WCE

/* DDE requests.  DDE uses an internal window for communication and so all
   DDE operations on a particular instance handle have to be performed by
   the same thread.  That thread also has to check and process the message
   queue.  The previous version did this by having the ML thread make the
   DDE calls and processed the message list in an "interrupt" routine.
   That complicates the Windows interface so now the ML thread sends messages
   to the main window thread to do the work. */
HCONV StartDDEConversation(char *serviceName, char *topicName)
{
    return (HCONV)SendMessage(hDDEWindow, WM_DDESTART, (WPARAM)serviceName, (LPARAM)topicName);
}

void CloseDDEConversation(HCONV hConv)
{
    SendMessage(hDDEWindow, WM_DDESTOP, 0, (LPARAM)hConv);
}

LRESULT ExecuteDDE(char *command, HCONV hConv)
{
    return SendMessage(hDDEWindow, WM_DDEEXEC, (WPARAM)hConv, (LPARAM)command);
}


LRESULT CALLBACK DDEWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg) 
    {
    case WM_DDESTART:
        {
            HCONV hcDDEConv;
            HSZ hszServiceName, hszTopicName;
            char *serviceName = (char*)wParam;
            char *topicName = (char*)lParam;
            hszServiceName =
                DdeCreateStringHandle(dwDDEInstance, serviceName, CP_WINANSI);
            hszTopicName =
                DdeCreateStringHandle(dwDDEInstance, topicName, CP_WINANSI);
            hcDDEConv =
                DdeConnect(dwDDEInstance, hszServiceName, hszTopicName, NULL);
            DdeFreeStringHandle(dwDDEInstance, hszServiceName);
            DdeFreeStringHandle(dwDDEInstance, hszTopicName);
            if (hcDDEConv == 0)
            {
                UINT nErr = DdeGetLastError(dwDDEInstance);
                return 0;
            }
            return (LRESULT)hcDDEConv;
        }

    case WM_DDESTOP:
        DdeDisconnect((HCONV)lParam);
        return 0;

    case WM_DDEEXEC:
        {
            HDDEDATA res;
            LPSTR   command = (LPSTR)lParam;
            res = DdeClientTransaction((LPBYTE)command, (DWORD)(strlen(command)+1),
                    (HCONV)wParam, 0L, 0, XTYP_EXECUTE, TIMEOUT_ASYNC, NULL);
            if (res != 0)
            {
                DdeFreeDataHandle(res);
                // Succeeded - return true;
                return 1;
            }
            else if (DdeGetLastError(dwDDEInstance) == DMLERR_BUSY)
                // If it's busy return false.
                return 0;
            else return -1; // An error
        }

    default:
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
}
#endif // _WIN32_WCE

/* In order to be able to handle all the keys we need to
   sub-class the edit control.  */ 
static LRESULT APIENTRY EditSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) 
{
    switch (uMsg)
    {
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS;

    case WM_KEYDOWN:
        switch(wParam)
        {
        case VK_DELETE: // Ignore the delete key.  Beep perhaps?
            return 0;
        case VK_LEFT: // If we move the cursor we are probably not
        case VK_RIGHT: // at the end.
        case VK_UP:
        case VK_DOWN:
            fAtEnd = FALSE;
        default:
            return CallWindowProc(wpOrigEditProc, hwnd, uMsg,  wParam, lParam);
        }

    case WM_CHAR:
        {
            LPARAM nRpt = lParam & 0xffff;
            EnterCriticalSection(&csIOInterlock);
            if (wParam == '\b')
            {
                // Delete the previous character(s).
                if (nNextPosn != nAvailable)
                {
                    int nCanRemove = 0;
                    while (nRpt-- > 0 && nNextPosn != nAvailable)
                    {
                        nCanRemove++;
                        if (nNextPosn == 0) nNextPosn = nBuffLen;
                        nNextPosn--;
                    }
                    lParam = (lParam & 0xffff0000) | nCanRemove;
                    LeaveCriticalSection(&csIOInterlock);
                    return CallWindowProc(wpOrigEditProc, hwnd, uMsg,  wParam, lParam); 
                }
            }
            else if (wParam == 22) // Control-V
            {
                // Generate a Paste command.
                SendMessage(hMainWindow, WM_COMMAND, ID_EDIT_PASTE, 0);
            }
            else if (wParam == 3) // Control-C
            {
                // In Windows this has the effect of Copy but we also
                // want it to generate an interrupt.  I've chosen to
                // make it copy if there is any selection, otherwise to
                // generate an interrupt.  We'll have to see how this works.
                DWORD dwStart, dwEnd;
                SendMessage(hwnd, EM_GETSEL, (WPARAM)&dwStart, (LPARAM)&dwEnd);
                if (dwStart != dwEnd)
                {
                    SendMessage(hwnd, WM_COPY, 0, 0); 
                }
                else {
                    // Discard any type-ahead.
                    nNextPosn = nAvailable = nReadPosn = 0;
                    RequestConsoleInterrupt();
                    // Set the ctrl-C flag and make sure the ML thread
                    // is unblocked.
                    fCtrlC = TRUE;
                    SetEvent(hInputEvent);
                }
            }
            else if (wParam >= ' ' || wParam == '\r' || wParam == '\t' ||
                     wParam == 4 /* ctrl-D */ || wParam == 26 /* ctrl-Z */)
            {
                CheckForBufferSpace(nRpt);
                CheckForScreenSpace(nRpt); // Make sure we have space on the screen.
                // Add the character(s) to the buffer.
                while (nRpt-- > 0)
                {
                    if (wParam == '\r')
                    {
                        pchInputBuffer[nNextPosn++] = '\n';
                        nAvailable = nNextPosn;
                        SetEvent(hInputEvent);
                    }
                    else if (wParam == 4 || wParam == 26)
                    {
                        // Treat either of these as EOF chars.
                        pchInputBuffer[nNextPosn++] = wParam;
                        nAvailable = nNextPosn;
                        SetEvent(hInputEvent);
                        wParam = 4;
                    }
                    else pchInputBuffer[nNextPosn++] = wParam;
                    if (nNextPosn == nBuffLen) nNextPosn = 0;
                    if (nAvailable == nBuffLen) nAvailable = 0;
                }
                MoveToEnd();
                LeaveCriticalSection(&csIOInterlock);
                // Add this to the window except if it's ctrl-Z or ctrl-D.
                if (wParam == 4 || wParam == 26) return 0;
                return CallWindowProc(wpOrigEditProc, hwnd, uMsg,  wParam, lParam); 
            }
            LeaveCriticalSection(&csIOInterlock);
            return 0;
        }

    case WM_DESTROY:
        {
            HFONT hFount;
            // Switch back to the old window proc just in case.
#ifdef _M_AMD64
            SetWindowLongPtr(hwnd, GWLP_WNDPROC, (INT_PTR)wpOrigEditProc);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
#else
            SetWindowLong(hwnd, GWL_WNDPROC, (LONG)wpOrigEditProc);
            SetWindowLong(hwnd, GWL_USERDATA, 0);
#endif
            // Get the fount and delete it if it's not the default.
            hFount = (HFONT)SendMessage(hwnd, WM_GETFONT, 0, 0);
            if (hFount != NULL)
            {
                SendMessage(hwnd, WM_SETFONT, (WPARAM)NULL, FALSE);
                DeleteObject(hFount);
            }
            // Call the original to finish off.
            return CallWindowProc(wpOrigEditProc, hwnd, uMsg,  wParam, lParam); 
        }

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case EM_SETSEL:
        // Need to record that we may no longer be at the end of the text.
        fAtEnd = FALSE;
        // Drop through to default.

    default:
        return CallWindowProc(wpOrigEditProc, hwnd, uMsg,  wParam, lParam); 
    }
} 

/* This function is only used when the "About Poly/ML" dialogue box is
   being displayed. */
static BOOL CALLBACK AboutProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_INITDIALOG: return 1;
    case WM_COMMAND:
        if (wParam == IDOK)
        {
            EndDialog(hwndDlg, IDOK);
        }
    default: return 0;
    }
}

#ifdef _WIN32_WCE
#define CF_TEXTFORMAT	CF_UNICODETEXT
#else
#define CF_TEXTFORMAT	CF_TEXT
#endif

/* This is the main window procedure. */
LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg) 
    { 
        case WM_CREATE:
            {
#ifdef _WIN32_WCE
		        hCmdBar = CommandBar_Create (hApplicationInstance, hwnd, 1);
		        CommandBar_InsertMenubar(hCmdBar, hApplicationInstance, IDR_MENU, 0);
		        CommandBar_AddAdornments(hCmdBar, 0, 0);
#endif
                hEditWnd = CreateWindow(_T("EDIT"), NULL,
                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                    ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL, 
                    0, 0, 0, 0,
                    hwnd, 0, hApplicationInstance, NULL);
                if (hEditWnd == NULL) return -1; /* Failed */

                // Sub-class this so that we get the keys that are pressed.
                // Save the old window proc.
#ifdef _M_AMD64
                wpOrigEditProc =
                    (WNDPROC)GetWindowLongPtr(hEditWnd, GWLP_WNDPROC);
               // Set our new window proc.
                SetWindowLongPtr(hEditWnd, GWLP_WNDPROC, (INT_PTR)EditSubclassProc);
#else
                wpOrigEditProc =
                    (WNDPROC)GetWindowLong(hEditWnd, GWL_WNDPROC);
               // Set our new window proc.
                SetWindowLong(hEditWnd, GWL_WNDPROC, (LONG)EditSubclassProc);
#endif
                fAtEnd = TRUE;

#ifndef _WIN32_WCE
                // Get a 10 point Courier fount.
                HDC hDC = GetDC(hEditWnd);
                int nHeight = -MulDiv(10, GetDeviceCaps(hDC, LOGPIXELSY), 72);
                ReleaseDC(hEditWnd, hDC);
                HFONT hFont = CreateFont(nHeight, 0, 0, 0, FW_DONTCARE,
                                    FALSE, FALSE, FALSE, ANSI_CHARSET,
                                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN,
                                    "Courier");
                if (hFont) SendMessage(hEditWnd, WM_SETFONT, (WPARAM)hFont, 0);
#endif
 
                SendMessage(hEditWnd, WM_SETTEXT, 0, (LPARAM) "");
                return 0; /* Succeeded */
            }
 
        case WM_SETFOCUS:
            /* When the focus arrives at the parent set the focus on
               the edit window. */
            SetFocus(hEditWnd); 
            return 0; 

        case WM_SIZE:
            {
                LONG offset = 0;
#ifdef _WIN32_WCE
                RECT rect;
                // Deduct the size of the command bar.
                if (GetWindowRect(hCmdBar, &rect)) offset = rect.bottom - rect.top;
                SendMessage(hCmdBar, TB_AUTOSIZE, 0L, 0L);
#endif
                // Make the edit control the size of the window's client area.
                MoveWindow(hEditWnd, 0, offset, LOWORD(lParam), HIWORD(lParam)-offset, TRUE);
            }
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        case WM_COMMAND:
            switch(wParam)
            {
            case ID_EDIT_COPY:
                SendMessage(hEditWnd, WM_COPY, 0, 0); 
                return 0; 

            case ID_EDIT_PASTE:
                if (IsClipboardFormatAvailable(CF_TEXTFORMAT))
                {
                    // We need to check that we have enough space
                    // BEFORE we try pasting.
                    HANDLE hClip;
                    LPCTSTR lpszText;
                    OpenClipboard(hEditWnd);
                    hClip = GetClipboardData(CF_TEXTFORMAT);
#ifdef _WIN32_WCE
					lpszText = (LPCTSTR)hClip;
#else
					lpszText = (LPCTSTR)GlobalLock(hClip);
#endif
                    CheckForScreenSpace(lstrlen(lpszText));
                    MoveToEnd();
                    // Add it to the screen.
                    SendMessage(hEditWnd, EM_REPLACESEL, FALSE, (LPARAM)lpszText);
                    // Add to the type-ahead.
                    EnterCriticalSection(&csIOInterlock);
                    // Check there's enough space.  This may be an
                    // over-estimate since we replace CRNL by NL.
                    CheckForBufferSpace(lstrlen(lpszText));
                    while (*lpszText)
                    {
                        // The data we're pasting contains CRNL as
                        // line separators.
                        if (lpszText[0] == '\r' && lpszText[1] == '\n')
                        {
                            pchInputBuffer[nNextPosn++] = '\n';
                            if (nNextPosn == nBuffLen) nNextPosn = 0;
                            nAvailable = nNextPosn;
                            lpszText += 2;
                        }
                        else {
                            pchInputBuffer[nNextPosn++] = (char)*lpszText++;
                            if (nNextPosn == nBuffLen) nNextPosn = 0;
                            if (lpszText[0] == 4 || lpszText[0] == 26)
                                nAvailable = nNextPosn; // EOF characters.
                        }
                    }
                    if (nAvailable != nReadPosn) SetEvent(hInputEvent);
                    LeaveCriticalSection(&csIOInterlock);
#ifndef _WIN32_WCE
					GlobalUnlock(hClip);
#endif
                    CloseClipboard();
                }
                return 0; 

            case ID_HELP_ABOUT: 
                DialogBox(hApplicationInstance, MAKEINTRESOURCE(IDD_ABOUT_POLYML),
                    hwnd, (DLGPROC)AboutProc); 
                return 0;

            case ID_FILE_QUIT:
                if (MessageBox(hwnd, _T("Are you sure you want to quit?"),
                                     _T("Confirm Quit"), MB_OKCANCEL) == IDOK)
                    RequestFinish(0);
                return 0;

            case ID_RUN_INTERRUPT:
                // Discard any type-ahead.
                nNextPosn = nAvailable = nReadPosn = 0;
                RequestConsoleInterrupt();
                // Set the ctrl-C flag and make sure the ML thread
                // is unblocked.
                fCtrlC = TRUE;
                SetEvent(hInputEvent);
                return 0;

            default: return DefWindowProc(hwnd, uMsg, wParam, lParam);
            }

        case WM_CLOSE:
            if (MessageBox(hwnd, _T("Are you sure you want to quit?"),
                                 _T("Confirm Quit"), MB_OKCANCEL) == IDOK)
                RequestFinish(0);
            return 0;


        case WM_ADDTEXT:
            // Request from the input thread to add some text.
            {
                // Remember the old selection and the original length.
                LRESULT lrStart, lrEnd;
                SendMessage(hEditWnd, EM_GETSEL,
                            (WPARAM)&lrStart, (LPARAM)&lrEnd);
                LRESULT lrLength = SendMessage(hEditWnd, WM_GETTEXTLENGTH, 0, 0);
                LRESULT lrRemoved = CheckForScreenSpace(lrLength);
                MoveToEnd();
                SendMessage(hEditWnd, EM_REPLACESEL, 0, lParam);
                // If the old selection was at the end (i.e. the pointer
                // was at the end) we don't reinstate the old selection.
                if (lrStart != lrLength && lrEnd > lrRemoved)
                {
                    if (lrStart > lrRemoved) lrStart -= lrRemoved; else lrStart = 0;
                    fAtEnd = FALSE;
                    SendMessage(hEditWnd, EM_SETSEL, lrStart, lrEnd-lrRemoved);
                }
                return 0;
            }


        default:
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
    } 
} 
 
#ifdef _WIN32_WCE
void writeToConsole(LPCTSTR text)
{
	SendMessage(hMainWindow, WM_ADDTEXT, 0, (LPARAM)text);
}

#else
static DWORD WINAPI InThrdProc(LPVOID lpParameter)
// This thread deals with input from the ML process.
{
    while (1)
    {
        CHAR buff[4096];
        DWORD dwRead;
        if (! ReadFile(hReadFromML, buff, sizeof(buff)-1, &dwRead, NULL))
            return 0;
        buff[dwRead] = 0;
        SendMessage(hMainWindow, WM_ADDTEXT, 0, (LPARAM)buff);
    }
}
#endif

static DWORD WINAPI MainThrdProc(LPVOID lpParameter)
// This thread simply continues with the rest of the ML
// initialistion.
{
    exportDescription *exports = (exportDescription *)lpParameter;
#ifdef _WIN32_WCE
	// We would need to convert the args from Unicode.  Don't bother at the moment.
    return polymain(0, 0, exports);
#else
    return polymain(nArgs, lpArgs, exports);
#endif
}

int PolyWinMain(
  HINSTANCE hInstance,
  HINSTANCE hPrevInstance,
  LPTSTR lpCmdLine,
  int nCmdShow,
  exportDescription *exports
)
{
    HANDLE hWriteToScreen = INVALID_HANDLE_VALUE;
    BOOL fNext = FALSE;
    DWORD dwInId, dwRes;

    InitializeCriticalSection(&csIOInterlock);
    hInputEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    hApplicationInstance = hInstance;

#ifdef _WIN32_WCE
    INITCOMMONCONTROLSEX icEx;
    memset(&icEx, 0, sizeof(icEx));
    icEx.dwSize = sizeof(icEx);
    icEx.dwICC = ICC_BAR_CLASSES;
    icEx.dwICC = ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icEx);
#endif

    // If we already have standard input and standard output we
    // don't replace them, otherwise we create a window and pipes
    // to connect it.  We use _get_osfhandle here because that
    // checks for handles passed in in the STARTUPINFO as well as
    // those inherited as standard handles.
#ifndef _WIN32_WCE
    if (_get_osfhandle(fileno(stdin)) == -1 ||
        _get_osfhandle(fileno(stdout)) == -1)
    {
        WNDCLASSEX wndClass;
#else
        WNDCLASS wndClass;
#endif
        ATOM atClass;
        // Allocate initial buffer space to maintain the invariants.
        nBuffLen = 80;
        pchInputBuffer = (char*)malloc(nBuffLen);

#ifndef _WIN32_WCE
        if (!CreatePipe(&hReadFromML, &hWriteToScreen, NULL, 0)) {
            return 1;
        }
        HANDLE hTemp;
        // The pipe handles we have are not inheritable.  We have to
        // make hWriteToScreen an inheritable handle so that
        // processes we fork using "system" or "_popen"
        // (used for profiling) can write to the screen.
        if (! DuplicateHandle(GetCurrentProcess(), hWriteToScreen,
                             GetCurrentProcess(), &hTemp, 0, TRUE, // inheritable
                             DUPLICATE_SAME_ACCESS )) {
            return 1;
        }
        CloseHandle(hWriteToScreen);
        hWriteToScreen = hTemp;

        // We never use stdin internally if we have our own console
        // but _pipe, (used in profiling) at least, needs stdin to
        // be non-empty.  Open it on NUL.
        fclose(stdin);
        int newstdin = open("NUL", _O_RDONLY);
        _dup2(newstdin, 0);
        // Open it for stdio as well.  Because the entries in the FILE table
        // are opened in order we need to do this to ensure that stdout and
        // stderr point to the correct entries.
        fdopen(0, "rt");
        SetStdHandle(STD_INPUT_HANDLE, (HANDLE)_get_osfhandle(newstdin));
        // Replace the standard Windows handles.
        SetStdHandle(STD_OUTPUT_HANDLE, hWriteToScreen);
        SetStdHandle(STD_ERROR_HANDLE, hWriteToScreen);
        // Close the stdio streams.  They may have been opened
        // on dummy handles.
        fclose(stdout);
        fclose(stderr);
        // Set up the new handles.
        int newstdout = _open_osfhandle ((INT_PTR)hWriteToScreen, _O_TEXT);
        if (newstdout != 1) _dup2(newstdout, 1);
        _dup2(newstdout, 2); // Stderr
        // Open for stdio.
        fdopen(1, "wt"); // == stdout
        fdopen(2, "wt"); // == stderr
        // Set stderr to unbuffered so that messages get written correctly.
        // (stdout is explicitly flushed).
        setvbuf(stderr, NULL, _IONBF, 0);

        // Create a thread to manage the output from ML.
        HANDLE hInThread = CreateThread(NULL, 0, InThrdProc, 0, 0, &dwInId);
        CloseHandle(hInThread);
        wndClass.cbSize = sizeof(wndClass);
#endif
        wndClass.style = 0;
        wndClass.lpfnWndProc = WndProc; 
        wndClass.cbClsExtra = 0;
        wndClass.cbWndExtra = 0; 
        wndClass.hInstance = hInstance; 
        wndClass.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON));
        wndClass.hCursor = NULL; // For the moment 
        wndClass.hbrBackground = NULL; // For the moment
        wndClass.lpszClassName = _T("PolyMLWindowClass");
#ifndef _WIN32_WCE
        wndClass.lpszMenuName = MAKEINTRESOURCE(IDR_MENU); 
		wndClass.hIconSm = NULL; // For the moment
		DWORD dwStyle = WS_OVERLAPPEDWINDOW;

        if ((atClass = RegisterClassEx(&wndClass)) == 0)
#else
		DWORD dwStyle = WS_VISIBLE;
		wndClass.lpszMenuName = NULL; // Not supported in WCE
		if ((atClass = RegisterClass(&wndClass)) == 0)
#endif
        {
            return 1;
        }

        hMainWindow = CreateWindow(
            (LPTSTR)atClass,
            _T("Poly/ML"),
            dwStyle,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            NULL,
            NULL,   // handle to menu or child-window identifier
            hInstance,
            NULL     // pointer to window-creation data
            );

        if (hMainWindow == NULL)
        {
            return 1;
        }

        ShowWindow(hMainWindow, nCmdShow);
#ifdef _WIN32_WCE
	    UpdateWindow(hMainWindow);
	    if (hCmdBar)
		    CommandBar_Show(hCmdBar, TRUE);
#endif
        useConsole = 1;
#ifndef _WIN32_WCE
    }
    else {
        // If we're not creating a window it's possible that we could still
        // call getConsoleInput if the handle underlying stdin is closed for
        // some reason.  Set the event so that we return EOF in that case.
        SetEvent(hInputEvent);
        useConsole = 0;
    }

    // Convert the command line into Unix-style arguments.
    LPTSTR lpCommandLine = GetCommandLine();
    nArgs = 0;
    while (*lpCommandLine != 0)
    {
        if (*lpCommandLine == '"')
        {
            // Treat quoted items as a whole.
            lpCommandLine++;
            lpArgs[nArgs++] = lpCommandLine;
            while (*lpCommandLine != 0 && *lpCommandLine != '"')
                lpCommandLine++;
            if (*lpCommandLine != 0) *lpCommandLine++ = 0;
        }
        else
        {
            // Not quoted - look for a separating space.
            lpArgs[nArgs++] = lpCommandLine;
            while (*lpCommandLine != 0 && *lpCommandLine != ' ')
                lpCommandLine++;
        }
        // Remove multiple spaces.
        while (*lpCommandLine == ' ') *lpCommandLine++ = 0;

        if (nArgs == sizeof(lpArgs)/sizeof(lpArgs[0])) break;

        // Extract the service name argument.
        if (strcmp(lpArgs[nArgs-1], "-pServiceName") == 0)
        {
            nArgs--;
            fNext = TRUE;
        }
        else {
            if (fNext) lpszServiceName = lpArgs[--nArgs];
            fNext = FALSE;
        }
    }

    // Create an internal hidden window to handle DDE requests from the ML thread.
    {
        WNDCLASSEX wndClass;
        ATOM atClass;
        memset(&wndClass, 0, sizeof(wndClass));
        wndClass.cbSize = sizeof(wndClass);
        wndClass.lpfnWndProc = DDEWndProc; 
        wndClass.hInstance = hInstance; 
        wndClass.lpszClassName = "PolyMLDDEWindowClass";

        if ((atClass = RegisterClassEx(&wndClass)) == 0) return 1;

        hDDEWindow = CreateWindow(
            (LPTSTR)atClass,
            "Poly/ML-DDE",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            NULL,
            NULL,   // handle to menu or child-window identifier
            hInstance,
            NULL     // pointer to window-creation data
            );
    }

    initDDEControl(lpszServiceName);
#endif

    // Call the main program to do the rest of the initialisation.
    hMainThread = CreateThread(NULL, 0, MainThrdProc, exports, 0, &dwInId);

    // Enter the main message loop.
    while (MsgWaitForMultipleObjects(1, &hMainThread,
                    FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0+1)
    {
        MSG Msg;
        while (PeekMessage(&Msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&Msg);
            DispatchMessage(&Msg);
        }
    }

    // Closing this end of the pipe will cause the thread to go away.
    if (hWriteToScreen != INVALID_HANDLE_VALUE) CloseHandle(hWriteToScreen);

    if (! GetExitCodeThread(hMainThread, &dwRes)) dwRes = 0;

    CloseHandle(hMainThread);
#ifndef _WIN32_WCE
    uninitDDEControl();
#endif
    DestroyWindow(hDDEWindow);
    DeleteCriticalSection(&csIOInterlock);
    if (hInputEvent) CloseHandle(hInputEvent);
    return dwRes;
}

#ifndef _WIN32_WCE

HDDEDATA CALLBACK DdeCallback(UINT uType, UINT uFmt, HCONV hconv,
                              HSZ hsz1, HSZ hsz2, HDDEDATA hdata,
                              DWORD dwData1, DWORD dwData2) 
{ 
    switch (uType) 
    { 
        case XTYP_CONNECT:
            // Client connecting.  For the moment we ignore
            // the topic.
            return (HDDEDATA) TRUE;

        case XTYP_EXECUTE:
            {
                // See what the message is.  The only ones we
                // handle are interrupt and terminate.
                CHAR buff[256];
                buff[0] = 0;
                DdeGetData(hdata, (LPBYTE)buff, sizeof(buff), 0);
                if (lstrcmpi(buff, POLYINTERRUPT) == 0)
                {
                    RequestConsoleInterrupt();
                    return (HDDEDATA) DDE_FACK;
                }
                if (lstrcmpi(buff, POLYTERMINATE) == 0)
                {
                    RequestFinish(0);
                    return (HDDEDATA) DDE_FACK;
                }
                return (HDDEDATA) DDE_FNOTPROCESSED;
            }

        default: 
            return (HDDEDATA) NULL; 
    } 
} 

static int initDDEControl(char *lpszName)
{
    HSZ hszServiceName;
    if (DdeInitialize(&dwDDEInstance, DdeCallback,
        APPCLASS_STANDARD | CBF_FAIL_ADVISES | CBF_FAIL_POKES |
        CBF_FAIL_REQUESTS | CBF_SKIP_ALLNOTIFICATIONS, 0)
        != DMLERR_NO_ERROR)
    return 0;

    // If we were given a service name we register that,
    // otherwise we use the default name.
    if (lpszName == 0) lpszName = POLYMLSERVICE;
    hszServiceName = DdeCreateStringHandle(dwDDEInstance, lpszName, CP_WINANSI);
    if (hszServiceName == 0) return 0;

    DdeNameService(dwDDEInstance, hszServiceName, 0L, DNS_REGISTER);

    DdeFreeStringHandle(dwDDEInstance, hszServiceName);
    return 1;
}

static void uninitDDEControl(void)
{
    // Unregister our name(s).
    DdeNameService(dwDDEInstance, 0L, 0L, DNS_UNREGISTER);
//  DdeAbandonTransaction(dwDDEInstance, 0L, 0L);
    // Unitialise DDE.
    DdeUninitialize(dwDDEInstance);
}

#endif
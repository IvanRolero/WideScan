#ifndef WINVER
#define WINVER 0x0600
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include "StdAfx.h"
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h> 
#include <malloc.h>
#include <math.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <vector>
#include <iostream>
#include <algorithm>
#include <string>
#include <sstream>
#include <iomanip>
#include <cstdio>
#include <cstring>
#include <ctype.h>

extern "C" {
#include <jpeglib.h>
}

#include "utils.h"
#include "ScannerAttributes.h"
#include "ctx_scan_2000.h"  
#include "SetWindowParams.h"
#include "resource.h"

// Hardware Alignment Allocators
void* _aligned_malloc(size_t size, size_t alignment) {
    void* ptr = malloc(size + alignment + sizeof(void*));
    if (!ptr) return NULL;
    void* aligned_ptr = (void*)(((uintptr_t)ptr + sizeof(void*) + alignment) & ~(alignment - 1));
    ((void**)aligned_ptr)[-1] = ptr;
    return aligned_ptr;
}

void _aligned_free(void* ptr) {
    if (ptr) {
        free(((void**)ptr)[-1]);
    }
}

// Global UI Settings Variables
int use_dpi = 200;     
int use_width = 1360;
int use_height = 20000;
int use_left = 0;
int use_top = 0;
bool bUseSRGB = true;
char g_BaseFilename[MAX_PATH] = "";
char g_OutputDir[MAX_PATH] = "";

// State Engines
HWND g_hMainDlg = NULL;
HWND g_hPreviewWnd = NULL;

// File-Streaming Engine Tracking Configurations
const char* g_PreviewTempFile = "widescan_preview.tmp";
HANDLE g_hPreviewWriteFile = INVALID_HANDLE_VALUE;

int g_PreviewWidth = 0;
volatile int g_PreviewHeight = 0;
volatile int g_PreviewYScale = 1; // Compensation factor for aspect ratio scaling
ScannerAttributes g_ScanAttr;
bool g_UnitReserved = false;

// Control Flags
volatile bool g_CancelBatch = false;
volatile bool g_IsScanning = false;

// Profile Configuration Engine (.INI Manager)
void GetIniPath(char* szPath, DWORD dwSize) {
    GetModuleFileName(NULL, szPath, dwSize);
    char* p = strrchr(szPath, '\\');
    if (p) strcpy(p + 1, "settings.ini");
    else strcpy(szPath, "settings.ini");
}

void LoadSettings() {
    char iniPath[MAX_PATH];
    GetIniPath(iniPath, MAX_PATH);

    use_dpi = GetPrivateProfileInt("ScannerSettings", "DPI", 200, iniPath);
    use_width = GetPrivateProfileInt("ScannerSettings", "Width", 1360, iniPath);
    use_height = GetPrivateProfileInt("ScannerSettings", "Height", 20000, iniPath);
    use_left = GetPrivateProfileInt("ScannerSettings", "Left", 0, iniPath);
    use_top = GetPrivateProfileInt("ScannerSettings", "Top", 0, iniPath);
    bUseSRGB = GetPrivateProfileInt("ScannerSettings", "UseSRGB", 1, iniPath);

    GetPrivateProfileString("ScannerSettings", "OutputDir", "", g_OutputDir, sizeof(g_OutputDir), iniPath);
    GetPrivateProfileString("ScannerSettings", "BaseFilename", "ScanOutput", g_BaseFilename, sizeof(g_BaseFilename), iniPath);
}

void SaveSettings() {
    char iniPath[MAX_PATH];
    GetIniPath(iniPath, MAX_PATH);
    char buf[32];

    snprintf(buf, sizeof(buf), "%d", use_dpi);  WritePrivateProfileString("ScannerSettings", "DPI", buf, iniPath);
    snprintf(buf, sizeof(buf), "%d", use_width); WritePrivateProfileString("ScannerSettings", "Width", buf, iniPath);
    snprintf(buf, sizeof(buf), "%d", use_height);WritePrivateProfileString("ScannerSettings", "Height", buf, iniPath);
    snprintf(buf, sizeof(buf), "%d", use_left); WritePrivateProfileString("ScannerSettings", "Left", buf, iniPath);
    snprintf(buf, sizeof(buf), "%d", use_top);  WritePrivateProfileString("ScannerSettings", "Top", buf, iniPath);
    WritePrivateProfileString("ScannerSettings", "UseSRGB", bUseSRGB ? "1" : "0", iniPath);
    WritePrivateProfileString("ScannerSettings", "OutputDir", g_OutputDir, iniPath);
    WritePrivateProfileString("ScannerSettings", "BaseFilename", g_BaseFilename, iniPath);
}

// Append tracking statements inside GUI log viewport
void LogToUI(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    HWND hLog = GetDlgItem(g_hMainDlg, IDC_EDIT_LOG);
    if (hLog) {
        int length = GetWindowTextLength(hLog);
        SendMessage(hLog, EM_SETSEL, length, length);
        SendMessage(hLog, EM_REPLACESEL, FALSE, (LPARAM)buffer);
        SendMessage(hLog, EM_REPLACESEL, FALSE, (LPARAM)"\r\n");
    }
}

int ReadAttributes(HSCANNER hs) {
    BYTE inqPageBuffer[255];
    if (S_OK != scanInquiry(hs, inqPageBuffer, sizeof(inqPageBuffer))) return FALSE;
    g_ScanAttr.scannerId.assign((const char *)inqPageBuffer + 16, 16);
    g_ScanAttr.vendorId.assign((const char *)inqPageBuffer + 8, 8);
    BYTE inqPagesSupBuffer[255];
    scanInquiryPage(hs, inqPagesSupBuffer, sizeof(inqPagesSupBuffer), SCAN_INQUIRY_PAGE_PAGES_SUPPORTED);
    int listLength = inqPagesSupBuffer[3];
    for (int listIndex = 1; listIndex < listLength; listIndex++) {
        if (S_OK == scanInquiryPage(hs, inqPageBuffer, sizeof(inqPageBuffer), inqPagesSupBuffer[4 + listIndex])) {
            if (inqPageBuffer[1] == SCAN_INQUIRY_PAGE_AUTO_CONFIG) {
                g_ScanAttr.centerLoad = GET_BYTE(inqPageBuffer, 11);
                g_ScanAttr.NoOfCameras = GET_DWORD(inqPageBuffer, 16);
                g_ScanAttr.maxScanWidth = GET_DWORD(inqPageBuffer, 28);
                g_ScanAttr.SizeOfGammaTable = 256; 
                g_ScanAttr.maxSetWindowLength = GET_BYTE(inqPageBuffer, 66);
            }
            if (inqPageBuffer[1] == SCAN_INQUIRY_PAGE_COLOR || inqPageBuffer[1] == SCAN_INQUIRY_PAGE_FIXED_RESOLUTION) {
                g_ScanAttr.SizeOfGammaTable = GET_DWORD(inqPageBuffer, 36);
            }
        }
    }
    return TRUE;
}

void CloseAndExit(HSCANNER hs) {
    if (g_hPreviewWriteFile != INVALID_HANDLE_VALUE) {
        CloseHandle(g_hPreviewWriteFile);
        g_hPreviewWriteFile = INVALID_HANDLE_VALUE;
    }
    if (g_UnitReserved) scanReleaseUnit(hs);
    if (hs > 0) scanCloseScanner(hs);
    scanCloseLib();
    g_UnitReserved = false;
    g_IsScanning = false;
    EnableWindow(GetDlgItem(g_hMainDlg, IDC_BTN_PREVIEW), TRUE);
    EnableWindow(GetDlgItem(g_hMainDlg, IDC_BTN_BATCH), TRUE);
    EnableWindow(GetDlgItem(g_hMainDlg, IDC_BTN_CANCEL), FALSE);
}

void MakeGammaTable(BYTE * pTab, int nReqSize, bool bUseSRGBVar) {
    for (int i = 0; i < nReqSize; i++) {
        pTab[i] = 255 * i / (nReqSize - 1);
        if (bUseSRGBVar)
            pTab[i] = (int)(0.5 + 255.0 * (((double)pTab[i] / 255.0 <= 0.0034) ? 
                      (12.92 * (double)pTab[i] / 255.0) : 
                      (1.055 * pow((double)pTab[i] / 255.0, 1 / 2.4) - 0.055)));
    }
}

void SetupColorScan(int width, int length, int dpi, bool bCenterLoad, SETWINDOWPARAMS & swp) {
    memset(&swp, 0, sizeof(swp));
    swp.m_ParameterListLength = g_ScanAttr.maxSetWindowLength - WINDOW_DESC_OFFSET;
    swp.m_dpix = dpi;
    swp.m_dpiy = dpi;
    swp.m_upperLeftX = (long)MM_TO_INCHDIV1200(use_left);
    swp.m_upperLeftY = (long)MM_TO_INCHDIV1200(use_top);
    swp.m_width = (long)MM_TO_INCHDIV1200(width);
    swp.m_length = (long)MM_TO_INCHDIV1200(length);

    if (bCenterLoad) swp.m_upperLeftX = g_ScanAttr.maxScanWidth / 2 - swp.m_width / 2;
    swp.m_threshold = 100;
    swp.m_imageComposition = 5;
    swp.m_colorComposition = 4;
    swp.m_bitsPerPixel = 24;
    swp.m_compressionType = 0x0;
    swp.m_lineLimit = 1000;
    swp.m_scanSpeed = 100;
    swp.m_ColorSpaceType = 1;
    swp.m_ColorSaturationLevel = 100;
    swp.Convert();
}

// Live Preview Window Proc Engine (With Dynamic Virtual V-Scroll Pipeline) needs fix for very large media
LRESULT CALLBACK PreviewWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static int s_vScrollPos = 0;

    switch (msg) {
    case WM_VSCROLL: {
        SCROLLINFO si = { sizeof(SCROLLINFO), SIF_ALL };
        GetScrollInfo(hwnd, SB_VERT, &si);
        int oldPos = si.nPos;

        switch (LOWORD(wParam)) {
            case SB_TOP:        si.nPos = si.nMin; break;
            case SB_BOTTOM:     si.nPos = si.nMax; break;
            case SB_LINEUP:     si.nPos -= 15; break;
            case SB_LINEDOWN:   si.nPos += 15; break;
            case SB_PAGEUP:     si.nPos -= si.nPage; break;
            case SB_PAGEDOWN:   si.nPos += si.nPage; break;
            case SB_THUMBTRACK: si.nPos = si.nTrackPos; break;
        }

        if (si.nPos < si.nMin) si.nPos = si.nMin;
        if (si.nPos > (int)(si.nMax - si.nPage)) si.nPos = si.nMax - si.nPage;
        if (si.nPos < 0) si.nPos = 0;

        si.fMask = SIF_POS;
        SetScrollInfo(hwnd, SB_VERT, &si, TRUE);

        if (si.nPos != oldPos) {
            ScrollWindowEx(hwnd, 0, oldPos - si.nPos, NULL, NULL, NULL, NULL, SW_INVALIDATE | SW_ERASE);
            UpdateWindow(hwnd);
        }
        return 0;
    }

case WM_SIZE: {
        if (g_PreviewWidth > 0 && g_PreviewHeight > 0) {
            RECT rect;
            GetClientRect(hwnd, &rect);
            
            // Use the current scale factor (1 for Solo, 4 for Batch) to find the true virtual height
            int currentYScale = (g_IsScanning && !g_CancelBatch && g_PreviewYScale > 0) ? g_PreviewYScale : 1;
            int destHeight = (g_PreviewHeight * currentYScale * rect.right) / g_PreviewWidth;

            SCROLLINFO si = { sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE };
            si.nMin = 0;
            si.nMax = destHeight;
            si.nPage = rect.bottom;
            SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
        }
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rect;
        GetClientRect(hwnd, &rect);

        int currentHeight = g_PreviewHeight;
        int currentYScale = g_PreviewYScale; 
        if (currentYScale <= 0) currentYScale = 1;
        
        if (g_PreviewWidth > 0 && currentHeight > 0 && rect.right > 0) {
            int destWidth = rect.right;
            int destHeight = (currentHeight * currentYScale * rect.right) / g_PreviewWidth;

            SCROLLINFO si = { sizeof(SCROLLINFO), SIF_POS | SIF_PAGE };
            GetScrollInfo(hwnd, SB_VERT, &si);
            int yScroll = si.nPos;

            int actualDrawHeight = rect.bottom;
            if (yScroll + actualDrawHeight > destHeight) {
                actualDrawHeight = destHeight - yScroll;
            }

            if (actualDrawHeight > 0 && destWidth > 0) {
                // Perfect Fractional Mapping: Convert screen yScroll to exact file row indices
                double srcYStartExact = ((double)yScroll * g_PreviewWidth) / (rect.right * currentYScale);
                double srcYEndExact = ((double)(yScroll + actualDrawHeight) * g_PreviewWidth) / (rect.right * currentYScale);

                int srcYStart = (int)floor(srcYStartExact);
                int srcYEnd = (int)ceil(srcYEndExact);

                if (srcYStart < 0) srcYStart = 0;
                if (srcYEnd > currentHeight) srcYEnd = currentHeight;
                int actualSrcHeight = srcYEnd - srcYStart;

                if (actualSrcHeight > 0) {
                    int rowBytes = g_PreviewWidth * 3;
                    int stride = (rowBytes + 3) & ~3;
                    DWORD bytesToRead = actualSrcHeight * stride;

                    std::vector<BYTE> displayBuf(bytesToRead);
                    HANDLE hRead = CreateFile(g_PreviewTempFile, GENERIC_READ, 
                                              FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, 
                                              OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY, NULL);
                    if (hRead != INVALID_HANDLE_VALUE) {
                        LARGE_INTEGER liOffset;
                        liOffset.QuadPart = (LONGLONG)srcYStart * stride;
                        SetFilePointerEx(hRead, liOffset, NULL, FILE_BEGIN);
                        DWORD bytesRead = 0;
                        ReadFile(hRead, displayBuf.data(), bytesToRead, &bytesRead, NULL);
                        CloseHandle(hRead);

                        BITMAPINFO bmi = {0};
                        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                        bmi.bmiHeader.biWidth = g_PreviewWidth;
                        bmi.bmiHeader.biHeight = -actualSrcHeight; // Top-down DIB layout
                        bmi.bmiHeader.biPlanes = 1;
                        bmi.bmiHeader.biBitCount = 24;
                        bmi.bmiHeader.biCompression = BI_RGB;

                        SetStretchBltMode(hdc, HALFTONE);
                        SetBrushOrgEx(hdc, 0, 0, NULL);
                        
                        // Calculate precise sub-pixel destination offsets to prevent rendering drift
                        int destYOffset = (int)(((double)srcYStart * rect.right * currentYScale) / g_PreviewWidth) - yScroll;
                        int destHeightScaled = (int)(((double)actualSrcHeight * rect.right * currentYScale) / g_PreviewWidth);

                        StretchDIBits(hdc, 0, destYOffset, destWidth, destHeightScaled,
                                      0, 0, g_PreviewWidth, actualSrcHeight,
                                      displayBuf.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
                    }
                }
            }

            // Cleanly fill any remaining background space below the image canvas
            if (actualDrawHeight < rect.bottom) {
                RECT bgRect = { 0, actualDrawHeight, rect.right, rect.bottom };
                FillRect(hdc, &bgRect, (HBRUSH)GetStockObject(LTGRAY_BRUSH));
            }
        } else {
            FillRect(hdc, &rect, (HBRUSH)GetStockObject(LTGRAY_BRUSH));
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        //ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

bool ValidateUIData(HWND hDlg) {
    // 1. Validate Width
    if (use_width < 21 || use_width > 1370) {
        MessageBox(hDlg, "Width (mm) must be a value between 21 and 1370.", "Error", MB_ICONWARNING);
        return false;
    }
    
    // 2. Validate Height
    if (use_height < 2000 || use_height > 30000) {
        MessageBox(hDlg, "Height (mm) must be a value between 2000 and 30000.", "Error", MB_ICONWARNING);
        return false;
    }

    // 3. Validate Filename length
    if (strlen(g_BaseFilename) == 0) {
        MessageBox(hDlg, "Serial number cannot by empty.", "Error", MB_ICONWARNING);
        return false;
    }

    // 4. Validate Alphanumeric and "-" rules for the filename
    for (int i = 0; g_BaseFilename[i] != '\0'; i++) {
        char c = g_BaseFilename[i];
        if (!isalnum(c) && c != '-') {
            MessageBox(hDlg, "Only alphanumeric characters (a-z, 0-9) and '-' symbol are allowed in Serial number field", "Error", MB_ICONWARNING);
            return false;
        }
    }
    
    return true;
}

void SyncUIData() {
    use_dpi = GetDlgItemInt(g_hMainDlg, IDC_EDIT_DPI, NULL, FALSE);
    use_width = GetDlgItemInt(g_hMainDlg, IDC_EDIT_WIDTH, NULL, FALSE);
    use_height = GetDlgItemInt(g_hMainDlg, IDC_EDIT_HEIGHT, NULL, FALSE);
    use_left = GetDlgItemInt(g_hMainDlg, IDC_EDIT_LEFT, NULL, FALSE);
    use_top = GetDlgItemInt(g_hMainDlg, IDC_EDIT_TOP, NULL, FALSE);
//    bUseSRGB = (IsDlgButtonChecked(g_hMainDlg, IDC_CHECK_SRGB) == BST_CHECKED);
    GetDlgItemText(g_hMainDlg, IDC_EDIT_PATH, g_OutputDir, sizeof(g_OutputDir));
    GetDlgItemText(g_hMainDlg, IDC_EDIT_FILENAME, g_BaseFilename, sizeof(g_BaseFilename));

    SaveSettings(); // Auto-flush configurations to local disk configuration standard
}


// Thread worker: Live Scan Preview Solo Mode (Optimized with 4x Subsampling)
DWORD WINAPI SamplePreviewThread(LPVOID lpParam) {
    (void)lpParam; 
    HSCANNER hs = -1;
    BOOL bIsOpen;
    
    LogToUI("Starting Live Preview Stream Mode...");
    if (scanOpenLib() != S_OK) { LogToUI("Error: scanOpenLib failed."); CloseAndExit(-1); return 1; }
    if (scanGetNextScanner(&hs, &bIsOpen, TRUE) != S_OK) { LogToUI("Error: Scanner not detected."); CloseAndExit(-1); return 1; }
    if (scanOpenScanner(hs) != S_OK) { LogToUI("Error: scanOpenScanner failed."); CloseAndExit(-1); return 1; }
    ReadAttributes(hs);
    if (scanReserveUnit(hs) != S_OK) { LogToUI("Error: Unit Reservation Conflict."); CloseAndExit(hs); return 1; }
    g_UnitReserved = true;
    LogToUI("Loading media transport layers...");
    if (scanObjectPosition(hs, SCAN_OBJ_POS_LOAD, 0) != S_OK) { CloseAndExit(hs); return 1; }

    BYTE statusBuf[128];
    int nBytesReceived = 0;
    bool bIsLoading = true;
    while (bIsLoading) {
        if (g_CancelBatch) { LogToUI("Scan canceled."); CloseAndExit(hs); return 0; }
        int rc = scanRead(hs, statusBuf, 2, SCAN_READSEND_CODE_SCANNER_STATUS, SCAN_READSEND_QUALIFIER_SCANNER_STATUS, &nBytesReceived);
        if (rc == 0 && nBytesReceived == 2 && statusBuf[1] == 0x30) bIsLoading = false;
        else Sleep(10);
    }

    SETWINDOWPARAMS swp;
    SetupColorScan(use_width, use_height, use_dpi, (g_ScanAttr.centerLoad == 1), swp);
    if (g_ScanAttr.maxSetWindowLength > (int)offsetof(SETWINDOWPARAMS, m_PostScanOriginalHandling)) {
        swp.m_PostScanOriginalHandling = SCAN_OBJ_POST_POS_EJECT_BACK;
    }

    BYTE *pSetWindowBuffer = (BYTE*)_aligned_malloc(g_ScanAttr.maxSetWindowLength, 8);
    memset(pSetWindowBuffer, 0, g_ScanAttr.maxSetWindowLength);
    memcpy(pSetWindowBuffer, &swp, std::min((int)sizeof(swp), g_ScanAttr.maxSetWindowLength));
    scanSetWindow(hs, pSetWindowBuffer, g_ScanAttr.maxSetWindowLength);
    _aligned_free(pSetWindowBuffer);

    BYTE *gammaBuf = new BYTE[3 * g_ScanAttr.SizeOfGammaTable];
    MakeGammaTable(gammaBuf, g_ScanAttr.SizeOfGammaTable, bUseSRGB);
    MakeGammaTable(gammaBuf + g_ScanAttr.SizeOfGammaTable, g_ScanAttr.SizeOfGammaTable, bUseSRGB);
    MakeGammaTable(gammaBuf + 2 * g_ScanAttr.SizeOfGammaTable, g_ScanAttr.SizeOfGammaTable, bUseSRGB);
    scanSend(hs, gammaBuf, 3 * g_ScanAttr.SizeOfGammaTable, SCAN_READSEND_CODE_GAMMA, SCAN_READSEND_QUALIFIER_GAMMA);
    delete[] gammaBuf;

    BYTE bwPointBuffer[24] = {0};
    DWORD wp = (bUseSRGB) ? 0 : 180;
    std::memcpy(bwPointBuffer + 0, &wp, sizeof(DWORD));
    std::memcpy(bwPointBuffer + 4, &wp, sizeof(DWORD));
    std::memcpy(bwPointBuffer + 8, &wp, sizeof(DWORD));
    scanSend(hs, bwPointBuffer, 24, SCAN_READSEND_CODE_BWPOINT, SCAN_READSEND_QUALIFIER_LINEARIZE_WORD);

    BYTE tmp = 0;
    if (scanScan(hs, &tmp, 1) != S_OK) { CloseAndExit(hs); return 1; }

    DWORD dwActualBytesPerLine = 0;
    int nBytesRead;
    BYTE pbCameraBuf[16];
    if (scanRead(hs, pbCameraBuf, 2 * g_ScanAttr.NoOfCameras, 0xFF, 0x02, &nBytesRead) == 0) {
        for (int i = 0; i < g_ScanAttr.NoOfCameras; i++) dwActualBytesPerLine += GET_WORD(pbCameraBuf, i * 2);
    }
    
    int iPixels = (int)(dwActualBytesPerLine / 3);
    g_PreviewWidth = iPixels;
    g_PreviewHeight = 0;
    g_PreviewYScale = 1; // Solo preview captures every row (1:1 scale)

    // Open clean transactional temporary file for live tracking previews
    g_hPreviewWriteFile = CreateFile(g_PreviewTempFile, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 
                                     NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);

    int iBytesToRead = iPixels * 3 * 20; 
    BYTE *pBuffer = (BYTE*)_aligned_malloc(iBytesToRead, 8);
    int iBytesRead = 0, total_rows = 0;
    std::vector<BYTE> residualBuffer;

    ShowWindow(g_hPreviewWnd, SW_SHOW);
    const int rowBytes = iPixels * 3;
    const int stride = (rowBytes + 3) & ~3;

    int rcStream = S_OK;
    do {
        if (g_CancelBatch) { LogToUI("Stream transmission aborted."); break; }
        rcStream = scanRead(hs, pBuffer, iBytesToRead, SCAN_READSEND_CODE_IMAGE, SCAN_READSEND_QUALIFIER_IMAGE, &iBytesRead);
        if (rcStream == SCSI_STATUS_GOOD && iBytesRead > 0) {
            residualBuffer.insert(residualBuffer.end(), pBuffer, pBuffer + iBytesRead);
            while (residualBuffer.size() >= (size_t)rowBytes) {
                BYTE* rowStart = residualBuffer.data();
                for (int i = 0; i < rowBytes; i += 3) {
                    BYTE tempRed = rowStart[i];
                    rowStart[i] = rowStart[i + 2];
                    rowStart[i + 2] = tempRed;
                }
                
                // Pack direct single row matrix directly out onto disk
                std::vector<BYTE> paddedRow(stride, 0);
                memcpy(paddedRow.data(), rowStart, rowBytes);
                DWORD written = 0;
                WriteFile(g_hPreviewWriteFile, paddedRow.data(), stride, &written, NULL);

                residualBuffer.erase(residualBuffer.begin(), residualBuffer.begin() + rowBytes);
                total_rows++;
                g_PreviewHeight = total_rows;
            }

            // Broadcast scrolling bounds metric synchronization messages
            RECT rc;
            GetClientRect(g_hPreviewWnd, &rc);
            int destHeight = (g_PreviewHeight * rc.right) / g_PreviewWidth;
            SCROLLINFO si = { sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE };
            si.nMin = 0; si.nMax = destHeight; si.nPage = rc.bottom;
            SetScrollInfo(g_hPreviewWnd, SB_VERT, &si, TRUE);

            InvalidateRect(g_hPreviewWnd, NULL, FALSE);
            
            MSG UImsg;
            while (PeekMessage(&UImsg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&UImsg);
                DispatchMessage(&UImsg);
            }
        }
    } while (rcStream == S_OK);

    LogToUI("Preview Finished. Processed Rows: %d", total_rows);
    _aligned_free(pBuffer);
    scanObjectPosition(hs, 0, 0xFFFFFF);
    CloseAndExit(hs);
    return 0;
}



// Thread worker: Batch SimpleScan Automation
DWORD WINAPI SimpleScanBatchThread(LPVOID lpParam) {
    (void)lpParam;
    HSCANNER hs = -1;
    BOOL bIsOpen;

    LogToUI("Initializing High-Speed Automated Batch Scans...");
    if (scanOpenLib() != S_OK) { LogToUI("Error: Core Initialization failed."); CloseAndExit(-1); return 1; }
    if (scanGetNextScanner(&hs, &bIsOpen, TRUE) != S_OK) { LogToUI("Error: Scanner lookup failure."); CloseAndExit(-1); return 1; }
    if (scanOpenScanner(hs) != S_OK) { LogToUI("Error: Handshake failed."); CloseAndExit(-1); return 1; }
    ReadAttributes(hs);
    if (scanReserveUnit(hs) != S_OK) { LogToUI("Error: Reservation conflict."); CloseAndExit(hs); return 1; }
    g_UnitReserved = true;

    std::string base_name(g_BaseFilename);
    bool write_to_file_var = !base_name.empty();
    int batch_count = 0;

    char base_target_path[MAX_PATH * 2];
    if (strlen(g_OutputDir) > 0) {
        snprintf(base_target_path, sizeof(base_target_path), "%s\\%s", g_OutputDir, base_name.c_str());
    } else {
        snprintf(base_target_path, sizeof(base_target_path), "%s", base_name.c_str());
    }

    const char* rawTempFile = "widescan_raw.tmp";

    // Keep looping smoothly until the user explicitly signals to close/cancel the batch
    while (!g_CancelBatch) {
        LogToUI("--- Waiting for Document in Slot %d ---", batch_count + 1);
        
        bool mediaLoaded = false;
        
        // Loop indefinitely checking for paper until media is found OR user cancels the batch
        while (!mediaLoaded && !g_CancelBatch) {
            if (scanObjectPosition(hs, SCAN_OBJ_POS_LOAD, 0) != S_OK) {
                // Responsiveness Fix: Instead of sleeping 15 solid seconds, 
                // sleep for 500ms at a time and constantly check if g_CancelBatch became true
                for (int i = 0; i < 10 && !g_CancelBatch; i++) {
                    Sleep(50); 
                }
            } else {
                mediaLoaded = true;
            }
        }
        
        // Break out immediately if the user pressed the Close Batch button during the wait
        if (g_CancelBatch) break;

        BYTE buf[128];
        int nBytesReceived = 0;
        bool bIsLoading = true;
        while (bIsLoading && !g_CancelBatch) {
            if (scanRead(hs, buf, 2, SCAN_READSEND_CODE_SCANNER_STATUS, SCAN_READSEND_QUALIFIER_SCANNER_STATUS, &nBytesReceived) == 0 && nBytesReceived == 2) {
                if (buf[1] == 0x30) bIsLoading = false;
                else Sleep(2);
            } else {
                LogToUI("Critical status feedback loss.");
                CloseAndExit(hs);
                return 1;
            }
        }
        if (g_CancelBatch) break;
        
        SETWINDOWPARAMS swp;
        SetupColorScan(use_width, use_height, use_dpi, (g_ScanAttr.centerLoad == 1), swp);
        if (g_ScanAttr.maxSetWindowLength > (int)offsetof(SETWINDOWPARAMS, m_PostScanOriginalHandling)) {
            swp.m_PostScanOriginalHandling = SCAN_OBJ_POST_POS_EJECT_BACK;
        }

        BYTE *pSetWindowBuffer = new BYTE[g_ScanAttr.maxSetWindowLength];
        memset(pSetWindowBuffer, 0, g_ScanAttr.maxSetWindowLength);
        memcpy(pSetWindowBuffer, &swp, std::min((int)sizeof(swp), g_ScanAttr.maxSetWindowLength));
        scanSetWindow(hs, pSetWindowBuffer, g_ScanAttr.maxSetWindowLength);
        delete[] pSetWindowBuffer;

        BYTE *gammaBuf = new BYTE[3 * g_ScanAttr.SizeOfGammaTable];
        MakeGammaTable(gammaBuf, g_ScanAttr.SizeOfGammaTable, bUseSRGB);
        MakeGammaTable(gammaBuf + g_ScanAttr.SizeOfGammaTable, g_ScanAttr.SizeOfGammaTable, bUseSRGB);
        MakeGammaTable(gammaBuf + 2 * g_ScanAttr.SizeOfGammaTable, g_ScanAttr.SizeOfGammaTable, bUseSRGB);
        scanSend(hs, gammaBuf, 3 * g_ScanAttr.SizeOfGammaTable, SCAN_READSEND_CODE_GAMMA, SCAN_READSEND_QUALIFIER_GAMMA);
        delete[] gammaBuf;

        BYTE bwPointBuffer[24] = {0};
        DWORD wp = (bUseSRGB) ? 0 : 180;
        std::memcpy(bwPointBuffer + 0, &wp, sizeof(DWORD));
        std::memcpy(bwPointBuffer + 4, &wp, sizeof(DWORD));
        std::memcpy(bwPointBuffer + 8, &wp, sizeof(DWORD));
        scanSend(hs, bwPointBuffer, 24, SCAN_READSEND_CODE_BWPOINT, SCAN_READSEND_QUALIFIER_LINEARIZE_WORD);

        BYTE tmp = 0;
        if (scanScan(hs, &tmp, 1) != S_OK) { LogToUI("Execution failed."); break; }

        DWORD dwActualBytesPerLine = 0;
        int nBytesRead, iPixels = 0;
        int iCameraBufSize = 2 * g_ScanAttr.NoOfCameras;
        BYTE *pbCameraBuf = new BYTE[iCameraBufSize];
        if (scanRead(hs, pbCameraBuf, iCameraBufSize, 0xFF, 0x02, &nBytesRead) == 0) {
            for (int i = 0; i < g_ScanAttr.NoOfCameras; i++) dwActualBytesPerLine += GET_WORD(pbCameraBuf, i * 2);
            iPixels = (int)(dwActualBytesPerLine / 3);
        }
        delete[] pbCameraBuf;

        g_PreviewWidth = iPixels;
        g_PreviewHeight = 0;
        g_PreviewYScale = 4; 
        ShowWindow(g_hPreviewWnd, SW_SHOW);

        HANDLE hRawFile = CreateFile(rawTempFile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
        g_hPreviewWriteFile = CreateFile(g_PreviewTempFile, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 
                                         NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);

        int iBytesToRead = iPixels * 3 * 60; 
        int iBytesRead = 0, total_rows = 0, saved_preview_rows = 0;
        std::vector<BYTE> residualBuffer;
        BYTE *pBuffer = (BYTE*)_aligned_malloc(iBytesToRead, 8);

        const int rowBytes = iPixels * 3;
        const int stride = (rowBytes + 3) & ~3;

        int rcScan = S_OK;
        do {
            if (g_CancelBatch) break;
            rcScan = scanRead(hs, pBuffer, iBytesToRead, SCAN_READSEND_CODE_IMAGE, SCAN_READSEND_QUALIFIER_IMAGE, &iBytesRead);
            if (rcScan == SCSI_STATUS_GOOD && iBytesRead > 0) {
                DWORD written = 0;
                WriteFile(hRawFile, pBuffer, iBytesRead, &written, NULL);

                residualBuffer.insert(residualBuffer.end(), pBuffer, pBuffer + iBytesRead);
                while (residualBuffer.size() >= (size_t)rowBytes) {
                    BYTE* rowStart = residualBuffer.data();
                    
                    if (total_rows % 4 == 0) {
                        for (int i = 0; i < rowBytes; i += 3) {
                            BYTE tempRed = rowStart[i];
                            rowStart[i] = rowStart[i + 2];
                            rowStart[i + 2] = tempRed;
                        }
                        
                        std::vector<BYTE> paddedRow(stride, 0);
                        memcpy(paddedRow.data(), rowStart, rowBytes);
                        WriteFile(g_hPreviewWriteFile, paddedRow.data(), stride, &written, NULL);
                        saved_preview_rows++;
                        g_PreviewHeight = saved_preview_rows;
                    }

                    residualBuffer.erase(residualBuffer.begin(), residualBuffer.begin() + rowBytes);
                    total_rows++;
                }

                RECT rc;
                GetClientRect(g_hPreviewWnd, &rc);
                int destHeight = (g_PreviewHeight * g_PreviewYScale * rc.right) / g_PreviewWidth;
                SCROLLINFO si = { sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE };
                
                si.nMin = 0; si.nMax = destHeight; si.nPage = rc.bottom;
                SetScrollInfo(g_hPreviewWnd, SB_VERT, &si, TRUE);

                InvalidateRect(g_hPreviewWnd, NULL, FALSE);
                MSG UImsg;
                while (PeekMessage(&UImsg, NULL, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&UImsg);
                    DispatchMessage(&UImsg);
                }
            }
        } while (rcScan == S_OK);
        
        _aligned_free(pBuffer);
        CloseHandle(hRawFile);
        if (g_hPreviewWriteFile != INVALID_HANDLE_VALUE) {
            CloseHandle(g_hPreviewWriteFile);
            g_hPreviewWriteFile = INVALID_HANDLE_VALUE;
        }

        if (g_CancelBatch) {
            LogToUI("Batch Operation Aborted mid-stream.");
            DeleteFile(rawTempFile);
            scanObjectPosition(hs, 0, 0xFFFFFF);
            break;
        }

        if (write_to_file_var && total_rows > 0) {
            char dynamic_filename[MAX_PATH * 2 + 32];
            snprintf(dynamic_filename, sizeof(dynamic_filename), "%s-%02d.jpg", base_target_path, batch_count + 1);

            struct jpeg_compress_struct cinfo;
            struct jpeg_error_mgr jerr;
            cinfo.err = jpeg_std_error(&jerr);
            jpeg_create_compress(&cinfo);
            FILE *outfile = fopen(dynamic_filename, "wb");
            if (outfile) {
                HANDLE hRawRead = CreateFile(rawTempFile, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY, NULL);
                if (hRawRead != INVALID_HANDLE_VALUE) {
                    jpeg_stdio_dest(&cinfo, outfile);
                    cinfo.image_width = iPixels;
                    cinfo.image_height = total_rows;
                    cinfo.input_components = 3;
                    cinfo.in_color_space = JCS_RGB;
                    jpeg_set_defaults(&cinfo);
                    jpeg_set_quality(&cinfo, 100, TRUE);
                    cinfo.density_unit = 1;
                    cinfo.X_density = use_dpi;
                    cinfo.Y_density = use_dpi;

                    jpeg_start_compress(&cinfo, TRUE);
                    int row_stride = iPixels * 3;
                    std::vector<BYTE> row_buffer(row_stride);

                    while (cinfo.next_scanline < cinfo.image_height) {
                        DWORD bytesRead = 0;
                        ReadFile(hRawRead, row_buffer.data(), row_stride, &bytesRead, NULL);
                        JSAMPROW row_pointer[1] = {(JSAMPROW)row_buffer.data()};
                        jpeg_write_scanlines(&cinfo, row_pointer, 1);
                    }
                    jpeg_finish_compress(&cinfo);
                    CloseHandle(hRawRead);
                }
                fclose(outfile);
                LogToUI("Saved Matrix Payload: %s", dynamic_filename);
            }
            jpeg_destroy_compress(&cinfo);
            DeleteFile(rawTempFile);
        }
        
        scanObjectPosition(hs, 0, 0xFFFFFF);
        batch_count++;
        LogToUI("Document %d complete. Ejecting media...", batch_count);
        // The MessageBox and trailing code cutoff have been completely removed!
        for (int i = 0; i < 60 && !g_CancelBatch; i++) {
            Sleep(50); // 60 * 50ms = 3000ms (3 seconds)
        }
    }


        if (write_to_file_var && batch_count == 1) {
        char old_name[MAX_PATH * 2 + 32], new_name[MAX_PATH * 2 + 32];
        snprintf(old_name, sizeof(old_name), "%s-01.jpg", base_target_path);
        snprintf(new_name, sizeof(new_name), "%s.jpg", base_target_path);
        
       
        if (std::rename(old_name, new_name) == 0) {
            LogToUI("Single file batch detected. Renamed asset to: %s", new_name);
        } else {
            LogToUI("Warning: Failed to rename single-file asset (File might be locked or missing).");
        }
    } else {
        LogToUI("Batch closed successfully. Total files processed: %d", batch_count);
    }
    

    CloseAndExit(hs);
    return 0;
}



// Master UI Dialog Procedure Loop
INT_PTR CALLBACK MainDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
    (void)lParam;
    switch (message) {
case WM_INITDIALOG: { 
        g_hMainDlg = hDlg; // 
        LoadSettings(); // Initialize baseline configuration properties [cite: 178]

        // Populate DPI Combobox
        HWND hComboDpi = GetDlgItem(hDlg, IDC_EDIT_DPI);
        const int dpi_values[] = {100, 200, 300, 400, 500};
        int sel_index = 1; // Default to 200 if match isn't found
        
        for (int i = 0; i < 5; i++) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", dpi_values[i]);
            SendMessage(hComboDpi, CB_ADDSTRING, 0, (LPARAM)buf);
            if (dpi_values[i] == use_dpi) sel_index = i;
        }
        SendMessage(hComboDpi, CB_SETCURSEL, sel_index, 0);
        
        SetDlgItemInt(hDlg, IDC_EDIT_WIDTH, use_width, FALSE); 
        SetDlgItemInt(hDlg, IDC_EDIT_HEIGHT, use_height, FALSE); 
        SetDlgItemInt(hDlg, IDC_EDIT_LEFT, use_left, FALSE); 
        SetDlgItemInt(hDlg, IDC_EDIT_TOP, use_top, FALSE); 
        //CheckDlgButton(hDlg, IDC_CHECK_SRGB, bUseSRGB ? BST_CHECKED : BST_UNCHECKED); 
        SetDlgItemText(hDlg, IDC_EDIT_PATH, g_OutputDir); 
        SetDlgItemText(hDlg, IDC_EDIT_FILENAME, g_BaseFilename); 
        EnableWindow(GetDlgItem(hDlg, IDC_BTN_CANCEL), FALSE); 
        
        WNDCLASS wc = {0}; 
        wc.lpfnWndProc = PreviewWndProc; 
        wc.hInstance = GetModuleHandle(NULL); 
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = "ScannerPreviewClass"; 
        RegisterClass(&wc); // [cite: 181]

        // 1. Find our resource frame positioning layout parameters
        HWND hPlaceholder = GetDlgItem(hDlg, IDC_STATIC_PREVIEW);
        RECT rcPlaceholder;
        GetWindowRect(hPlaceholder, &rcPlaceholder);
        
        // 2. Map coordinates relative to the Main Dialog's client window space
        MapWindowPoints(NULL, hDlg, (LPPOINT)&rcPlaceholder, 2);
        
        // 3. Hide placeholder component boundary frame line
        ShowWindow(hPlaceholder, SW_HIDE);

        // 4. Instantiated embedded viewport as an inline Child element
        g_hPreviewWnd = CreateWindowEx(0, "ScannerPreviewClass", NULL,
                                      WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER,
                                      rcPlaceholder.left, 
                                      rcPlaceholder.top,
                                      rcPlaceholder.right - rcPlaceholder.left, 
                                      rcPlaceholder.bottom - rcPlaceholder.top, 
                                      hDlg, NULL, GetModuleHandle(NULL), NULL);
        return (INT_PTR)TRUE; 
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BTN_BROWSE: {
                    char szDir[MAX_PATH] = {0};
                    BROWSEINFO bi = {0};
                    bi.hwndOwner = hDlg;
                    bi.lpszTitle = "Select Output Directory Path for Scanned Content:";
                    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                    
                    // Changed from PIDLIST_ABSOLUTE to LPITEMIDLIST for total compatibility
                    LPITEMIDLIST pidl = SHBrowseForFolder(&bi); 
                    if (pidl != NULL) {
                        SHGetPathFromIDList(pidl, szDir);
                        SetDlgItemText(hDlg, IDC_EDIT_PATH, szDir);
                        CoTaskMemFree(pidl);
                    }
                    return (INT_PTR)TRUE;
                }

        case IDC_BTN_PREVIEW:
            SyncUIData();
            if (!ValidateUIData(hDlg)) return (INT_PTR)TRUE; 
            g_CancelBatch = false;
            g_IsScanning = true;
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_PREVIEW), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_BATCH), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_CANCEL), TRUE);
            CreateThread(NULL, 0, SamplePreviewThread, NULL, 0, NULL);
            return (INT_PTR)TRUE;

        case IDC_BTN_BATCH:
            SyncUIData();
            if (!ValidateUIData(hDlg)) return (INT_PTR)TRUE; 
            g_CancelBatch = false;
            g_IsScanning = true;
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_PREVIEW), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_BATCH), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_CANCEL), TRUE);
            CreateThread(NULL, 0, SimpleScanBatchThread, NULL, 0, NULL);
            return (INT_PTR)TRUE;

        case IDC_BTN_CANCEL:
            g_CancelBatch = true;
            LogToUI("Cancellation Signal Dispatched...");
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_CANCEL), FALSE);
            SetDlgItemText(g_hMainDlg, IDC_EDIT_FILENAME, "");
            return (INT_PTR)TRUE;

        case IDCANCEL:
            if (g_IsScanning) {
                g_CancelBatch = true;
                Sleep(500);
            }
            DestroyWindow(g_hPreviewWnd);
            DeleteFile(g_PreviewTempFile); 
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hInstance; (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow;
    DialogBox(hInstance, MAKEINTRESOURCE(IDD_MAIN_DIALOG), NULL, MainDlgProc);
    return 0;
}
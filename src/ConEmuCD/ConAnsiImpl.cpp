﻿
/*
Copyright (c) 2012-present Maximus5
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. The name of the authors may not be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ''AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


#include "../common/defines.h"
#include <WinError.h>
#include <WinNT.h>
#include <TCHAR.h>
#include <limits>
#include "../common/Common.h"
#include "../common/ConEmuCheck.h"
#include "../common/CmdLine.h"
#include "../common/ConsoleAnnotation.h"
#include "../common/UnicodeChars.h"
#include "../common/WConsole.h"
#include "../common/WErrGuard.h"
#include "../ConEmu/version.h"

#include "ConAnsiImpl.h"
#include "ConEmuSrv.h"



#define ANSI_MAP_CHECK_TIMEOUT 1000

#ifdef _DEBUG
#define DebugString(x) OutputDebugString(x)
#define DebugStringA(x) OutputDebugStringA(x)
#else
#define DebugString(x) //OutputDebugString(x)
#define DebugStringA(x) //OutputDebugStringA(x)
#endif

#ifdef DUMP_UNKNOWN_ESCAPES
#define DumpUnknownEscape(buf, cchLen) m_Owner->DumpEscape(buf, cchLen, SrvAnsi::de_Unknown)
#define DumpKnownEscape(buf, cchLen, eType) m_Owner->DumpEscape(buf, cchLen, eType)
#else
#define DumpUnknownEscape(buf,cchLen)
#define DumpKnownEscape(buf, cchLen, eType)
#endif



SrvAnsiImpl::SrvAnsiImpl(SrvAnsi* _owner, condata::Table* _table)
	: m_UseLock(_owner->m_UseMutex)
	, m_Owner(_owner)
	, m_Table(_table)
{
	m_Owner->GetFeatures(NULL, &m_Owner->mb_SuppressBells);
}

SrvAnsiImpl::~SrvAnsiImpl()
{
}


bool SrvAnsiImpl::OurWriteConsole(const wchar_t* lpBuffer, DWORD nNumberOfCharsToWrite, LPDWORD lpNumberOfCharsWritten)
{
	bool lbRc = false;

	// For debugger breakpoint
	m_Owner->FirstAnsiCall((const BYTE*)lpBuffer, nNumberOfCharsToWrite);
	// In debug builds: Write to debug console all console Output
	DumpKnownEscape((wchar_t*)lpBuffer, nNumberOfCharsToWrite, SrvAnsi::de_Normal);

	// Logging?
	if (lpBuffer && nNumberOfCharsToWrite && m_Owner->ghAnsiLogFile)
	{
		m_Owner->WriteAnsiLogW(lpBuffer, nNumberOfCharsToWrite);
	}

	CEStr CpCvt;

	if (lpBuffer && nNumberOfCharsToWrite)
	{
		// if that was API call of WriteConsoleW
		if (m_Owner->gCpConv.nFromCP && m_Owner->gCpConv.nToCP)
		{
			// Convert from Unicode to MBCS
			int iMBCSLen = WideCharToMultiByte(m_Owner->gCpConv.nFromCP, 0, (LPCWSTR)lpBuffer, nNumberOfCharsToWrite, NULL, 0, NULL, NULL);
			if (iMBCSLen > 0)
			{
				CEStrA szTemp;
				if (char* pszTemp = szTemp.getbuffer(iMBCSLen))
				{
					BOOL bFailed = FALSE; // Do not do conversion if some chars can't be mapped
					iMBCSLen = WideCharToMultiByte(m_Owner->gCpConv.nFromCP, 0, (LPCWSTR)lpBuffer, nNumberOfCharsToWrite, pszTemp, iMBCSLen, NULL, &bFailed);
					if ((iMBCSLen > 0) && !bFailed)
					{
						int iWideLen = MultiByteToWideChar(m_Owner->gCpConv.nToCP, 0, pszTemp, iMBCSLen, NULL, 0);
						if (iWideLen > 0)
						{
							if (wchar_t* ptrBuf = CpCvt.GetBuffer(iWideLen))
							{
								iWideLen = MultiByteToWideChar(m_Owner->gCpConv.nToCP, 0, pszTemp, iMBCSLen, ptrBuf, iWideLen);
								if (iWideLen > 0)
								{
									lpBuffer = ptrBuf;
									nNumberOfCharsToWrite = iWideLen;
								}
							}
						}
					}
				}
			}
		}
	}

	_ASSERTE(m_Owner->m_BellsCounter == 0);
	m_Owner->m_BellsCounter = 0;

	// The output
	lbRc = WriteAnsiCodes(lpBuffer, nNumberOfCharsToWrite, lpNumberOfCharsWritten);

	// Bells counter?
	if (m_Owner->m_BellsCounter)
	{
		// User may disable flashing in ConEmu settings
		CESERVER_REQ *pIn = ExecuteNewCmd(CECMD_FLASHWINDOW, sizeof(CESERVER_REQ_HDR)+sizeof(CESERVER_REQ_FLASHWINFO));
		if (pIn)
		{
			ExecutePrepareCmd(pIn, CECMD_FLASHWINDOW, sizeof(CESERVER_REQ_HDR)+sizeof(CESERVER_REQ_FLASHWINFO)); //-V119
			pIn->Flash.fType = eFlashBeep;
			pIn->Flash.hWnd = ghConWnd;
			pIn->Flash.bInvert = FALSE;
			pIn->Flash.dwFlags = FLASHW_ALL;
			pIn->Flash.uCount = 1;
			pIn->Flash.dwTimeout = 0;
			auto pOut = ExecuteGuiCmd(ghConWnd, pIn, ghConWnd);
			if (pOut) ExecuteFreeResult(pOut);
			ExecuteFreeResult(pIn);
		}
		m_Owner->m_BellsCounter = 0;
	}
	return lbRc;
}

bool SrvAnsiImpl::WriteText(LPCWSTR lpBuffer, DWORD nNumberOfCharsToWrite, LPDWORD lpNumberOfCharsWritten)
{
	DWORD /*nWritten = 0,*/ nTotalWritten = 0;

	if (lpBuffer && nNumberOfCharsToWrite)
		m_Owner->m_LastWrittenChar = lpBuffer[nNumberOfCharsToWrite-1];

	LPCWSTR pszSrcBuffer = lpBuffer;
	wchar_t cvtBuf[400], *pcvtBuf = NULL; CEStr szTemp;
	if (m_Owner->mCharSet && lpBuffer && nNumberOfCharsToWrite)
	{
		static wchar_t G0_DRAWING[31] = {
			0x2666 /*♦*/, 0x2592 /*▒*/, 0x2192 /*→*/, 0x21A8 /*↨*/, 0x2190 /*←*/, 0x2193 /*↓*/, 0x00B0 /*°*/, 0x00B1 /*±*/,
			0x00B6 /*¶*/, 0x2195 /*↕*/, 0x2518 /*┘*/, 0x2510 /*┐*/, 0x250C /*┌*/, 0x2514 /*└*/, 0x253C /*┼*/, 0x203E /*‾*/,
			0x207B /*⁻*/, 0x2500 /*─*/, 0x208B /*₋*/, 0x005F /*_*/, 0x251C /*├*/, 0x2524 /*┤*/, 0x2534 /*┴*/, 0x252C /*┬*/,
			0x2502 /*│*/, 0x2264 /*≤*/, 0x2265 /*≥*/, 0x03C0 /*π*/, 0x2260 /*≠*/, 0x00A3 /*£*/, 0x00B7 /*·*/
		};
		LPCWSTR pszMap = NULL;
		switch (m_Owner->mCharSet)
		{
		case SrvAnsi::VTCS_DRAWING:
			pszMap = G0_DRAWING;
			break;
		}
		if (pszMap)
		{
			wchar_t* dst = NULL;
			for (DWORD i = 0; i < nNumberOfCharsToWrite; ++i)
			{
				if (pszSrcBuffer[i] >= 0x60 && pszSrcBuffer[i] < 0x7F)
				{
					if (!pcvtBuf)
					{
						if (nNumberOfCharsToWrite <= countof(cvtBuf))
						{
							pcvtBuf = cvtBuf;
						}
						else
						{
							if (!(pcvtBuf = szTemp.GetBuffer(nNumberOfCharsToWrite)))
								break;
						}
						lpBuffer = pcvtBuf;
						dst = pcvtBuf;
						if (i)
							memmove_s(dst, nNumberOfCharsToWrite * sizeof(*dst), pszSrcBuffer, i * sizeof(*dst));
					}
					dst[i] = pszMap[pszSrcBuffer[i] - 0x60];
				}
				else if (dst)
				{
					dst[i] = pszSrcBuffer[i];
				}
			}
		}
	}

	m_Table->Write(lpBuffer, nNumberOfCharsToWrite);

	if (lpNumberOfCharsWritten)
		*lpNumberOfCharsWritten = nNumberOfCharsToWrite;

	return true;
}

//struct AnsiEscCode
//{
//	wchar_t  First;  // ESC (27)
//	wchar_t  Second; // any of 64 to 95 ('@' to '_')
//	wchar_t  Action; // any of 64 to 126 (@ to ~). this is terminator
//	wchar_t  Skip;   // Если !=0 - то эту последовательность нужно пропустить
//	int      ArgC;
//	int      ArgV[16];
//	LPCWSTR  ArgSZ; // Reserved for key mapping
//	size_t   cchArgSZ;
//
//#ifdef _DEBUG
//	LPCWSTR  pszEscStart;
//	size_t   nTotalLen;
//#endif
//
//	int      PvtLen;
//	wchar_t  Pvt[16];
//};


// 0 - нет (в lpBuffer только текст)
// 1 - в Code помещена Esc последовательность (может быть простой текст ДО нее)
// 2 - нет, но кусок последовательности сохранен в gsPrevAnsiPart
int SrvAnsiImpl::NextEscCode(LPCWSTR lpBuffer, LPCWSTR lpEnd, wchar_t (&szPreDump)[SrvAnsi::CEAnsi_MaxPrevPart], DWORD& cchPrevPart, LPCWSTR& lpStart, LPCWSTR& lpNext, SrvAnsiImpl::AnsiEscCode& Code, bool ReEntrance /*= FALSE*/)
{
	int iRc = 0;
	wchar_t wc;

	LPCWSTR lpSaveStart = lpBuffer;
	lpStart = lpBuffer;

	_ASSERTEX(cchPrevPart==0);

	auto& gsPrevAnsiPart = m_Owner->gsPrevAnsiPart;
	auto& gnPrevAnsiPart = m_Owner->gnPrevAnsiPart;
	auto& gsPrevAnsiPart2 = m_Owner->gsPrevAnsiPart2;
	auto& gnPrevAnsiPart2 = m_Owner->gnPrevAnsiPart2;

	if (gnPrevAnsiPart && !ReEntrance)
	{
		if (*gsPrevAnsiPart == 27)
		{
			_ASSERTEX(gnPrevAnsiPart < 79);
			ssize_t nCurPrevLen = gnPrevAnsiPart;
			ssize_t nAdd = std::min((lpEnd-lpBuffer),(ssize_t)countof(gsPrevAnsiPart)-nCurPrevLen-1);
			// Need to check buffer overflow!!!
			_ASSERTEX((ssize_t)countof(gsPrevAnsiPart)>(nCurPrevLen+nAdd));
			wmemcpy(gsPrevAnsiPart+nCurPrevLen, lpBuffer, nAdd);
			gsPrevAnsiPart[nCurPrevLen+nAdd] = 0;

			WARNING("Проверить!!!");
			LPCWSTR lpReStart, lpReNext;
			int iCall = NextEscCode(gsPrevAnsiPart, gsPrevAnsiPart+nAdd+gnPrevAnsiPart, szPreDump, cchPrevPart, lpReStart, lpReNext, Code, TRUE);
			if (iCall == 1)
			{
				if ((lpReNext - gsPrevAnsiPart) >= gnPrevAnsiPart)
				{
					// Bypass unrecognized ESC sequences to screen?
					if (lpReStart > gsPrevAnsiPart)
					{
						ssize_t nSkipLen = (lpReStart - gsPrevAnsiPart); //DWORD nWritten;
						_ASSERTEX(nSkipLen>0 && nSkipLen<=countof(gsPrevAnsiPart) && nSkipLen<=gnPrevAnsiPart);
						DumpUnknownEscape(gsPrevAnsiPart, nSkipLen);

						//WriteText(gsPrevAnsiPart, nSkipLen, &nWritten);
						_ASSERTEX(nSkipLen <= ((int)SrvAnsi::CEAnsi_MaxPrevPart - (int)cchPrevPart));
						memmove(szPreDump, gsPrevAnsiPart, nSkipLen);
						cchPrevPart += int(nSkipLen);

						if (nSkipLen < gnPrevAnsiPart)
						{
							memmove(gsPrevAnsiPart, lpReStart, (gnPrevAnsiPart - nSkipLen)*sizeof(*gsPrevAnsiPart));
							gnPrevAnsiPart -= nSkipLen;
						}
						else
						{
							_ASSERTEX(nSkipLen == gnPrevAnsiPart);
							*gsPrevAnsiPart = 0;
							gnPrevAnsiPart = 0;
						}
						lpReStart = gsPrevAnsiPart;
					}
					_ASSERTEX(lpReStart == gsPrevAnsiPart);
					lpStart = lpBuffer; // nothing to dump before Esc-sequence
					_ASSERTEX((lpReNext - gsPrevAnsiPart) >= gnPrevAnsiPart);
					WARNING("Проверить!!!");
					lpNext = lpBuffer + (lpReNext - gsPrevAnsiPart - gnPrevAnsiPart);
				}
				else
				{
					_ASSERTEX((lpReNext - gsPrevAnsiPart) >= gnPrevAnsiPart);
					lpStart = lpNext = lpBuffer;
				}
				gnPrevAnsiPart = 0;
				gsPrevAnsiPart[0] = 0;
				iRc = 1;
				goto wrap2;
			}
			else if (iCall == 2)
			{
				gnPrevAnsiPart = nCurPrevLen+nAdd;
				_ASSERTEX(gsPrevAnsiPart[nCurPrevLen+nAdd] == 0);
				iRc = 2;
				goto wrap;
			}

			_ASSERTEX((iCall == 1) && "Invalid esc sequence, need dump to screen?");
		}
		else
		{
			_ASSERTEX(*gsPrevAnsiPart == 27);
		}
	}


	while (lpBuffer < lpEnd)
	{
		switch (*lpBuffer)
		{
		case 27:
			{
				ssize_t nLeft;
				LPCWSTR lpEscStart = lpBuffer;

				#ifdef _DEBUG
				Code.pszEscStart = lpBuffer;
				Code.nTotalLen = 0;
				#endif

				// Special one char codes? Like "ESC 7" and so on...
				if ((lpBuffer + 1) < lpEnd)
				{
					// But it may be some "special" codes
					switch (lpBuffer[1])
					{
					case L'7': // Save xterm cursor
					case L'8': // Restore xterm cursor
					case L'c': // Full reset
					case L'=':
					case L'>':
					case L'M': // Reverse LF
					case L'E': // CR-LF
					case L'D': // LF
						// xterm?
						lpStart = lpEscStart;
						Code.First = 27;
						Code.Second = *(++lpBuffer);
						Code.ArgC = 0;
						Code.PvtLen = 0;
						Code.Pvt[0] = 0;
						lpEnd = (++lpBuffer);
						iRc = 1;
						goto wrap;
					}
				}

				// If tail is larger than 2 chars, continue
				if ((lpBuffer + 2) < lpEnd)
				{
					// Set lpSaveStart to current start of Esc sequence, it was set to beginning of buffer
					_ASSERTEX(lpSaveStart <= lpBuffer);
					lpSaveStart = lpBuffer;
					_ASSERTEX(lpSaveStart == lpEscStart);

					Code.First = 27;
					Code.Second = *(++lpBuffer);
					Code.ArgC = 0;
					Code.PvtLen = 0;
					Code.Pvt[0] = 0;

					TODO("Bypass unrecognized ESC sequences to screen? Don't try to eliminate 'Possible' sequences?");
					//if (((Code.Second < 64) || (Code.Second > 95)) && (Code.Second != 124/* '|' - vim-xterm-emulation */))
					if (!wcschr(L"[]|()%", Code.Second))
					{
						// Don't assert on rawdump of KeyEvents.exe Esc key presses
						// 10:00:00 KEY_EVENT_RECORD: Dn, 1, Vk="VK_ESCAPE" [27/0x001B], Scan=0x0001 uChar=[U='\x1b' (0x001B): A='\x1b' (0x1B)]
						bool bStandaloneEscChar = (lpStart < lpSaveStart) && ((*(lpSaveStart-1) == L'\'' && Code.Second == L'\'') || (*(lpSaveStart-1) == L' ' && Code.Second == L' '));
						//_ASSERTEX(bStandaloneEscChar && "Unsupported control sequence?");
						if (!bStandaloneEscChar)
						{
							DumpKnownEscape(Code.pszEscStart, std::min<size_t>(Code.nTotalLen, 32), SrvAnsi::de_UnkControl);
						}
						continue; // invalid code
					}

					// Теперь идут параметры.
					++lpBuffer; // переместим указатель на первый символ ЗА CSI (после '[')

					auto parseNumArgs = [&Code, lpSaveStart](const wchar_t* &lpBuffer, const wchar_t* lpSeqEnd, bool saveAction) -> bool
					{
						wchar_t wc;
						int nValue = 0, nDigits = 0;
						Code.ArgC = 0;

						while (lpBuffer < lpSeqEnd)
						{
							switch (*lpBuffer)
							{
							case L'0': case L'1': case L'2': case L'3': case L'4':
							case L'5': case L'6': case L'7': case L'8': case L'9':
								nValue = (nValue * 10) + (((int)*lpBuffer) - L'0');
								++nDigits;
								break;

							case L';':
								// Даже если цифр не было - default "0"
								if (Code.ArgC < (int)countof(Code.ArgV))
									Code.ArgV[Code.ArgC++] = nValue; // save argument
								nDigits = nValue = 0;
								break;

							default:
								if (Code.Second == L']')
								{
									// OSC specific, stop on first non-digit/non-semicolon
									if (nDigits && (Code.ArgC < (int)countof(Code.ArgV)))
										Code.ArgV[Code.ArgC++] = nValue;
									return (Code.ArgC > 0);
								}
								else if (((wc = *lpBuffer) >= 64) && (wc <= 126))
								{
									// Fin
									if (saveAction)
										Code.Action = wc;
									if (nDigits && (Code.ArgC < (int)countof(Code.ArgV)))
										Code.ArgV[Code.ArgC++] = nValue;
									return true;
								}
								else if (!nDigits && !Code.ArgC)
								{
									if ((Code.PvtLen+1) < (int)countof(Code.Pvt))
									{
										Code.Pvt[Code.PvtLen++] = wc; // Skip private symbols
										Code.Pvt[Code.PvtLen] = 0;
									}
								}
							}
							++lpBuffer;
						}

						if (nDigits && (Code.ArgC < (int)countof(Code.ArgV)))
							Code.ArgV[Code.ArgC++] = nValue;
						return (Code.Second == L']');
					};

					switch (Code.Second)
					{
					case L'(':
					case L')':
					case L'%':
					//case L'#':
					//case L'*':
					//case L'+':
					//case L'-':
					//case L'.':
					//case L'/':
						// VT G0/G1/G2/G3 character sets
						lpStart = lpSaveStart;
						Code.Action = *(lpBuffer++);
						Code.Skip = 0;
						Code.ArgSZ = NULL;
						Code.cchArgSZ = 0;
						lpEnd = lpBuffer;
						iRc = 1;
						goto wrap;
					case L'|':
						// vim-xterm-emulation
					case L'[':
						// Standard
						Code.Skip = 0;
						Code.ArgSZ = NULL;
						Code.cchArgSZ = 0;
						{
							#ifdef _DEBUG
							LPCWSTR pszSaveStart = lpBuffer;
							#endif

							if (parseNumArgs(lpBuffer, lpEnd, true))
							{
								lpStart = lpSaveStart;
								lpEnd = lpBuffer+1;
								iRc = 1;
								goto wrap;
							}
						}
						// В данном запросе (на запись) конца последовательности нет,
						// оставшийся хвост нужно сохранить в буфере, для следующего запроса
						// Ниже
						break;

					case L']':
						// Finalizing (ST) with "\x1B\\" or "\x07"
						// "%]4;16;rgb:00/00/00%\" - "%" is ESC
						// "%]0;this is the window titleBEL"
						// ESC ] 0 ; txt ST        Set icon name and window title to txt.
						// ESC ] 1 ; txt ST        Set icon name to txt.
						// ESC ] 2 ; txt ST        Set window title to txt.
						// ESC ] 4 ; num; txt ST   Set ANSI color num to txt.
						// ESC ] 10 ; txt ST       Set dynamic text color to txt.
						// ESC ] 4 6 ; name ST     Change log file to name (normally disabled
						//					       by a compile-time option)
						// ESC ] 5 0 ; fn ST       Set font to fn.
						//Following 2 codes - from linux terminal
						// ESC ] P nrrggbb         Set palette, with parameter given in 7
                        //                         hexadecimal digits after the final P :-(.
						//                         Here n is the color (0-15), and rrggbb indicates
						//                         the red/green/blue values (0-255).
						// ESC ] R                 reset palette

						// ConEmu specific
						// ESC ] 9 ; 1 ; ms ST           Sleep. ms - milliseconds
						// ESC ] 9 ; 2 ; "txt" ST        Show GUI MessageBox ( txt ) for dubug purposes
						// ESC ] 9 ; 3 ; "txt" ST        Set TAB text
						// ESC ] 9 ; 4 ; st ; pr ST      When _st_ is 0: remove progress. When _st_ is 1: set progress value to _pr_ (number, 0-100). When _st_ is 2: set error state in progress on Windows 7 taskbar
						// ESC ] 9 ; 5 ST                Wait for ENTER/SPACE/ESC. Set EnvVar "ConEmuWaitKey" to ENTER/SPACE/ESC on exit.
						// ESC ] 9 ; 6 ; "txt" ST        Execute GuiMacro. Set EnvVar "ConEmuMacroResult" on exit.
						// and others... look at SrvAnsiImpl::WriteAnsiCode_OSC

						Code.ArgSZ = lpBuffer;
						Code.cchArgSZ = 0;
						//Code.Skip = Code.Second;

						while (lpBuffer < lpEnd)
						{
							if ((lpBuffer[0] == 7) ||
								(lpBuffer[0] == 27) /* we'll check the proper terminator below */)
							{
								Code.Action = *Code.ArgSZ; // первый символ последовательности
								Code.cchArgSZ = (lpBuffer - Code.ArgSZ);
								lpStart = lpSaveStart;
								const wchar_t* lpBufferPtr = Code.ArgSZ;
								if (lpBuffer[0] == 27)
								{
									if ((lpBuffer + 1) >= lpEnd)
									{
										// Sequence is not complete yet!
										break;
									}
									else if (lpBuffer[1] == L'\\')
									{
										lpEnd = lpBuffer + 2;
									}
									else
									{
										lpEnd = lpBuffer - 1;
										_ASSERTE(*(lpEnd+1) == 27);
										DumpUnknownEscape(Code.pszEscStart,Code.nTotalLen);
										iRc = 0;
										goto wrap;
									}
								}
								else
								{
									lpEnd = lpBuffer + 1;
								}
								parseNumArgs(lpBufferPtr, lpBuffer, false);
								iRc = 1;
								goto wrap;
							}
							++lpBuffer;
						}
						// Sequence is not complete, we have to store it to concatenate
						// and check on future write call. Below.
						break;

					default:
						// Unknown sequence, use common termination rules
						Code.Skip = Code.Second;
						Code.ArgSZ = lpBuffer;
						Code.cchArgSZ = 0;
						while (lpBuffer < lpEnd)
						{
							// Terminator ASCII symbol: from `@` to `~`
							if (((wc = *lpBuffer) >= 64) && (wc <= 126))
							{
								Code.Action = wc;
								lpStart = lpSaveStart;
								lpEnd = lpBuffer+1;
								iRc = 1;
								goto wrap;
							}
							++lpBuffer;
						}

					} // end of "switch (Code.Second)"
				} // end of minimal length check

				if ((nLeft = (lpEnd - lpEscStart)) <= SrvAnsi::CEAnsi_MaxPrevAnsiPart)
				{
					if (ReEntrance)
					{
						//_ASSERTEX(!ReEntrance && "Need to be checked!"); -- seems to be OK

						// gsPrevAnsiPart2 stored for debug purposes only (fully excess)
						wmemmove(gsPrevAnsiPart2, lpEscStart, nLeft);
						gsPrevAnsiPart2[nLeft] = 0;
						gnPrevAnsiPart2 = nLeft;
					}
					else
					{
						wmemmove(gsPrevAnsiPart, lpEscStart, nLeft);
						gsPrevAnsiPart[nLeft] = 0;
						gnPrevAnsiPart = nLeft;
					}
				}
				else
				{
					_ASSERTEX(FALSE && "Too long Esc-sequence part, Need to be checked!");
				}

				lpStart = lpEscStart;

				iRc = 2;
				goto wrap;
			} // end of "case 27:"
			break;
		} // end of "switch (*lpBuffer)"

		++lpBuffer;
	} // end of "while (lpBuffer < lpEnd)"

wrap:
	lpNext = lpEnd;

	#ifdef _DEBUG
	if (iRc == 1)
		Code.nTotalLen = (lpEnd - Code.pszEscStart);
	#endif
wrap2:
	_ASSERTEX((iRc==0) || (lpStart>=lpSaveStart && lpStart<lpEnd));
	return iRc;
}

bool SrvAnsiImpl::ScrollScreen(int nDir)
{
	ExtScrollScreenParm scrl = {sizeof(scrl), essf_Current|essf_Commit, hConsoleOutput, nDir, {}, L' '};
	if (gDisplayOpt.ScrollRegion)
	{
		_ASSERTEX(gDisplayOpt.ScrollStart>=0 && gDisplayOpt.ScrollEnd>=gDisplayOpt.ScrollStart);
		scrl.Region.top = gDisplayOpt.ScrollStart;
		scrl.Region.bottom = gDisplayOpt.ScrollEnd;
		scrl.Flags |= essf_Region;
	}

	bool lbRc = ExtScrollScreen(&scrl);

	return lbRc;
}

bool SrvAnsiImpl::PadAndScroll(CONSOLE_SCREEN_BUFFER_INFO& csbi)
{
	bool lbRc = FALSE;
	COORD crFrom = {csbi.dwCursorPosition.X, csbi.dwCursorPosition.Y};
	DEBUGTEST(DWORD nCount = csbi.dwSize.X - csbi.dwCursorPosition.X);

	/*
	lbRc = FillConsoleOutputAttribute(hConsoleOutput, GetDefaultTextAttr(), nCount, crFrom, &nWritten)
		&& FillConsoleOutputCharacter(hConsoleOutput, L' ', nCount, crFrom, &nWritten);
	*/

	if ((csbi.dwCursorPosition.Y + 1) >= csbi.dwSize.Y)
	{
		lbRc = ScrollScreen(hConsoleOutput, -1);
		crFrom.X = 0;
	}
	else
	{
		crFrom.X = 0; crFrom.Y++;
		lbRc = TRUE;
	}

	csbi.dwCursorPosition = crFrom;
	m_Owner->SetConsoleCursorPosition(hConsoleOutput, crFrom);

	return lbRc;
}

bool SrvAnsiImpl::FullReset()
{
	CONSOLE_SCREEN_BUFFER_INFO csbi = {};
	if (!GetConsoleScreenBufferInfoCached(hConsoleOutput, &csbi))
		return FALSE;

	m_Owner->ReSetDisplayParm(TRUE, TRUE);

	// Easy way to drop all lines
	ScrollScreen(hConsoleOutput, -csbi.dwSize.Y);

	// Reset cursor
	COORD cr0 = {};
	m_Owner->SetConsoleCursorPosition(hConsoleOutput, cr0);

	//TODO? Saved cursor position?

	return TRUE;
}

bool SrvAnsiImpl::ForwardLF(bool& bApply)
{
	CONSOLE_SCREEN_BUFFER_INFO csbi = {};
	if (!GetConsoleScreenBufferInfoCached(hConsoleOutput, &csbi))
		return FALSE;

	if (bApply)
	{
		m_Owner->ReSetDisplayParm(FALSE, TRUE);
		bApply = FALSE;
	}

	if (csbi.dwCursorPosition.Y == (csbi.dwSize.Y - 1))
	{
		WriteText(pfnWriteConsoleW, hConsoleOutput, L"\n", 1, NULL);
		m_Owner->SetConsoleCursorPosition(hConsoleOutput, csbi.dwCursorPosition);
	}
	else if (csbi.dwCursorPosition.Y < (csbi.dwSize.Y - 1))
	{
		COORD cr = {csbi.dwCursorPosition.X, csbi.dwCursorPosition.Y + 1};
		m_Owner->SetConsoleCursorPosition(hConsoleOutput, cr);
		if (cr.Y > csbi.srWindow.Bottom)
		{
			SMALL_RECT rcNew = csbi.srWindow;
			rcNew.Bottom = cr.Y;
			rcNew.Top = cr.Y - (csbi.srWindow.Bottom - csbi.srWindow.Top);
			_ASSERTE(rcNew.Top >= 0);
			SetConsoleWindowInfo(hConsoleOutput, TRUE, &rcNew);
		}
	}
	else
	{
		_ASSERTE(csbi.dwCursorPosition.Y > 0);
	}

	return TRUE;
}

bool SrvAnsiImpl::ReverseLF(bool& bApply)
{
	CONSOLE_SCREEN_BUFFER_INFO csbi = {};
	if (!GetConsoleScreenBufferInfoCached(hConsoleOutput, &csbi))
		return FALSE;

	if (bApply)
	{
		m_Owner->ReSetDisplayParm(FALSE, TRUE);
		bApply = FALSE;
	}

	if ((csbi.dwCursorPosition.Y == csbi.srWindow.Top)
		|| (gDisplayOpt.ScrollRegion && csbi.dwCursorPosition.Y == gDisplayOpt.ScrollStart))
	{
		LinesInsert(hConsoleOutput, 1);
	}
	else if (csbi.dwCursorPosition.Y > 0)
	{
		COORD cr = {csbi.dwCursorPosition.X, csbi.dwCursorPosition.Y - 1};
		m_Owner->SetConsoleCursorPosition(hConsoleOutput, cr);
	}
	else
	{
		_ASSERTE(csbi.dwCursorPosition.Y > 0);
	}

	return TRUE;
}

bool SrvAnsiImpl::LinesInsert(const unsigned LinesCount)
{
	CONSOLE_SCREEN_BUFFER_INFO csbi = {};
	if (!GetConsoleScreenBufferInfoCached(hConsoleOutput, &csbi))
	{
		_ASSERTEX(FALSE && "GetConsoleScreenBufferInfoCached failed");
		return FALSE;
	}

	// Apply default color before scrolling!
	m_Owner->ReSetDisplayParm(FALSE, TRUE);

	bool lbRc = FALSE;

	int TopLine, BottomLine;
	if (gDisplayOpt.ScrollRegion)
	{
		_ASSERTEX(gDisplayOpt.ScrollStart>=0 && gDisplayOpt.ScrollEnd>=gDisplayOpt.ScrollStart);
		if (csbi.dwCursorPosition.Y < gDisplayOpt.ScrollStart || csbi.dwCursorPosition.Y > gDisplayOpt.ScrollEnd)
			return TRUE;
		TopLine = csbi.dwCursorPosition.Y;
		BottomLine = std::max<int>(gDisplayOpt.ScrollEnd, 0);

		if ((TopLine + LinesCount) <= BottomLine)
		{
			ExtScrollScreenParm scrl = {
				sizeof(scrl), essf_Current|essf_Commit|essf_Region, hConsoleOutput,
				LinesCount, {}, L' ',
				// region to be scrolled (that is not a clipping region)
				{0, TopLine, csbi.dwSize.X - 1, BottomLine}};
			lbRc |= ExtScrollScreen(&scrl);
		}
		else
		{
			ExtFillOutputParm fill = {
				sizeof(fill), efof_Attribute|efof_Character, hConsoleOutput,
				{}, L' ', {0, TopLine}, csbi.dwSize.X * LinesCount};
			lbRc |= ExtFillOutput(&fill);
		}
	}
	else
	{
		// What we need to scroll? Buffer or visible rect?
		TopLine = csbi.dwCursorPosition.Y;
		BottomLine = (csbi.dwCursorPosition.Y <= csbi.srWindow.Bottom)
			? csbi.srWindow.Bottom
			: csbi.dwSize.Y - 1;

		ExtScrollScreenParm scrl = {
			sizeof(scrl), essf_Current|essf_Commit|essf_Region, hConsoleOutput,
			LinesCount, {}, L' ', {0, TopLine, csbi.dwSize.X-1, BottomLine}};
		lbRc |= ExtScrollScreen(&scrl);
	}

	return lbRc;
}

bool SrvAnsiImpl::LinesDelete(const unsigned LinesCount)
{
	CONSOLE_SCREEN_BUFFER_INFO csbi = {};
	if (!GetConsoleScreenBufferInfoCached(hConsoleOutput, &csbi))
	{
		_ASSERTEX(FALSE && "GetConsoleScreenBufferInfoCached failed");
		return FALSE;
	}

	// Apply default color before scrolling!
	m_Owner->ReSetDisplayParm(FALSE, TRUE);

	bool lbRc = FALSE;

	int TopLine, BottomLine;
	if (gDisplayOpt.ScrollRegion)
	{
		_ASSERTEX(gDisplayOpt.ScrollStart>=0 && gDisplayOpt.ScrollEnd>gDisplayOpt.ScrollStart);
		// ScrollStart & ScrollEnd are 0-based absolute line indexes
		// relative to VISIBLE area, these are not absolute buffer coords
		if (((csbi.dwCursorPosition.Y + LinesCount) <= gDisplayOpt.ScrollStart)
			|| (csbi.dwCursorPosition.Y > gDisplayOpt.ScrollEnd))
			return TRUE; // Nothing to scroll
		TopLine = csbi.dwCursorPosition.Y;
		BottomLine = gDisplayOpt.ScrollEnd;

		ExtScrollScreenParm scrl = {
			sizeof(scrl), essf_Current|essf_Commit|essf_Region, hConsoleOutput,
			-int(LinesCount), {}, L' ', {0, TopLine, csbi.dwSize.X-1, BottomLine}};
		if (scrl.Region.top < gDisplayOpt.ScrollStart)
		{
			scrl.Region.top = gDisplayOpt.ScrollStart;
			scrl.Dir += (gDisplayOpt.ScrollStart - TopLine);
		}
		if ((scrl.Dir < 0) && (scrl.Region.top <= scrl.Region.bottom))
		{
			lbRc |= ExtScrollScreen(&scrl);
		}
	}
	else
	{
		// What we need to scroll? Buffer or visible rect?
		TopLine = csbi.dwCursorPosition.Y;
		BottomLine = (csbi.dwCursorPosition.Y <= csbi.srWindow.Bottom)
			? csbi.srWindow.Bottom
			: csbi.dwSize.Y - 1;

		if (BottomLine < TopLine)
		{
			_ASSERTEX(FALSE && "Invalid (empty) scroll region");
			return FALSE;
		}

		ExtScrollScreenParm scrl = {
			sizeof(scrl), essf_Current|essf_Commit|essf_Region, hConsoleOutput,
			-int(LinesCount), {}, L' ', {0, TopLine, csbi.dwSize.X-1, BottomLine}};
		lbRc |= ExtScrollScreen(&scrl);
	}

	return lbRc;
}

int SrvAnsiImpl::NextNumber(LPCWSTR& asMS)
{
	wchar_t wc;
	int ms = 0;
	while (((wc = *(asMS++)) >= L'0') && (wc <= L'9'))
		ms = (ms * 10) + (int)(wc - L'0');
	return ms;
}

// ESC ] 9 ; 1 ; ms ST           Sleep. ms - milliseconds
void SrvAnsiImpl::DoSleep(LPCWSTR asMS)
{
	int ms = NextNumber(asMS);
	if (!ms)
		ms = 100;
	else if (ms > 10000)
		ms = 10000;
	// Delay
	Sleep(ms);
}

void SrvAnsiImpl::EscCopyCtrlString(wchar_t* pszDst, LPCWSTR asMsg, ssize_t cchMaxLen)
{
	if (!pszDst)
	{
		_ASSERTEX(pszDst!=NULL);
		return;
	}

	if (cchMaxLen < 0)
	{
		_ASSERTEX(cchMaxLen >= 0);
		cchMaxLen = 0;
	}
	if (cchMaxLen > 1)
	{
		if ((asMsg[0] == L'"') && (asMsg[cchMaxLen-1] == L'"'))
		{
			asMsg++;
			cchMaxLen -= 2;
		}
	}

	if (cchMaxLen > 0)
		wmemmove(pszDst, asMsg, cchMaxLen);
	pszDst[std::max<ssize_t>(cchMaxLen, 0)] = 0;
}

// ESC ] 9 ; 2 ; "txt" ST          Show GUI MessageBox ( txt ) for dubug purposes
void SrvAnsiImpl::DoMessage(LPCWSTR asMsg, ssize_t cchLen)
{
	wchar_t* pszText = (wchar_t*)malloc((cchLen+1)*sizeof(*pszText));

	if (pszText)
	{
		EscCopyCtrlString(pszText, asMsg, cchLen);
		//if (cchLen > 0)
		//	wmemmove(pszText, asMsg, cchLen);
		//pszText[cchLen] = 0;

		wchar_t szExe[MAX_PATH] = {};
		GetModuleFileName(NULL, szExe, countof(szExe));
		wchar_t szTitle[MAX_PATH+64];
		msprintf(szTitle, countof(szTitle), L"PID=%u, %s", GetCurrentProcessId(), PointToName(szExe));

		GuiMessageBox(ghConEmuWnd, pszText, szTitle, MB_ICONINFORMATION|MB_SYSTEMMODAL);

		free(pszText);
	}
}

bool SrvAnsiImpl::IsAnsiExecAllowed(LPCWSTR asCmd)
{
	// Invalid command or macro?
	if (!asCmd || !*asCmd)
		return false;

	// We need to check settings
	CESERVER_CONSOLE_MAPPING_HDR* pMap = m_Owner->GetConMap();
	if (!pMap)
		return false;

	if ((pMap->Flags & CECF_AnsiExecAny) != 0)
	{
		// Allowed in any process
	}
	else if ((pMap->Flags & CECF_AnsiExecCmd) != 0)
	{
		// Allowed in Cmd.exe only
		if (!gbIsCmdProcess)
			return false;
	}
	else
	{
		// Disallowed everywhere
		return false;
	}

	// Now we need to ask GUI, if the command (asCmd) is allowed
	bool bAllowed = false;
	ssize_t cchLen = wcslen(asCmd) + 1;
	CESERVER_REQ* pOut = NULL;
	CESERVER_REQ* pIn = ExecuteNewCmd(CECMD_ALLOWANSIEXEC, sizeof(CESERVER_REQ_HDR)+sizeof(wchar_t)*cchLen);

	if (pIn)
	{
		_ASSERTE(sizeof(pIn->wData[0])==sizeof(*asCmd));
		memmove(pIn->wData, asCmd, cchLen*sizeof(pIn->wData[0]));

		pOut = ExecuteGuiCmd(ghConWnd, pIn, ghConWnd);
		if (pOut && (pOut->DataSize() == sizeof(pOut->dwData[0])))
		{
			bAllowed = (pOut->dwData[0] == TRUE);
		}
	}

	ExecuteFreeResult(pOut);
	ExecuteFreeResult(pIn);

	return bAllowed;
}

// ESC ] 9 ; 6 ; "macro" ST        Execute some GuiMacro
void SrvAnsiImpl::DoGuiMacro(LPCWSTR asCmd, ssize_t cchLen)
{
	CESERVER_REQ* pOut = NULL;
	CESERVER_REQ* pIn = ExecuteNewCmd(CECMD_GUIMACRO, sizeof(CESERVER_REQ_HDR)+sizeof(CESERVER_REQ_GUIMACRO)+sizeof(wchar_t)*(cchLen + 1));

	if (pIn)
	{
		EscCopyCtrlString(pIn->GuiMacro.sMacro, asCmd, cchLen);

		if (IsAnsiExecAllowed(pIn->GuiMacro.sMacro))
		{
			pOut = ExecuteGuiCmd(ghConWnd, pIn, ghConWnd);
		}
	}

	// EnvVar "ConEmuMacroResult"
	SetEnvironmentVariable(CEGUIMACRORETENVVAR, pOut && pOut->GuiMacro.nSucceeded ? pOut->GuiMacro.sMacro : NULL);

	ExecuteFreeResult(pOut);
	ExecuteFreeResult(pIn);
}

// ESC ] 9 ; 7 ; "cmd" ST        Run some process with arguments
void SrvAnsiImpl::DoProcess(LPCWSTR asCmd, ssize_t cchLen)
{
	// We need zero-terminated string
	wchar_t* pszCmdLine = (wchar_t*)malloc((cchLen + 1)*sizeof(*asCmd));

	if (pszCmdLine)
	{
		EscCopyCtrlString(pszCmdLine, asCmd, cchLen);

		if (IsAnsiExecAllowed(pszCmdLine))
		{
			STARTUPINFO si = {sizeof(si)};
			PROCESS_INFORMATION pi = {};

			bool bCreated = OnCreateProcessW(NULL, pszCmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
			if (bCreated)
			{
				WaitForSingleObject(pi.hProcess, INFINITE);
				CloseHandle(pi.hProcess);
				CloseHandle(pi.hThread);
			}
		}

		free(pszCmdLine);
	}
}

// ESC ] 9 ; 8 ; "env" ST        Output value of environment variable
void SrvAnsiImpl::DoPrintEnv(LPCWSTR asCmd, ssize_t cchLen)
{
	if (!pfnWriteConsoleW)
		return;

	// We need zero-terminated string
	wchar_t* pszVarName = (wchar_t*)malloc((cchLen + 1)*sizeof(*asCmd));

	if (pszVarName)
	{
		EscCopyCtrlString(pszVarName, asCmd, cchLen);

		wchar_t szValue[MAX_PATH];
		wchar_t* pszValue = szValue;
		DWORD cchMax = countof(szValue);
		DWORD nMax = GetEnvironmentVariable(pszVarName, pszValue, cchMax);

		// Some predefined as `time`, `date`, `cd`, ...
		if (!nMax)
		{
			if ((lstrcmpi(pszVarName, L"date") == 0)
				|| (lstrcmpi(pszVarName, L"time") == 0))
			{
				SYSTEMTIME st = {}; GetLocalTime(&st);
				if (lstrcmpi(pszVarName, L"date") == 0)
					swprintf_c(szValue, L"%u-%02u-%02u", st.wYear, st.wMonth, st.wDay);
				else
					swprintf_c(szValue, L"%u:%02u:%02u.%03u", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
				nMax = lstrlen(szValue);
			}
			#if 0
			else if (lstrcmpi(pszVarName, L"cd") == 0)
			{
				//TODO: If possible
			}
			#endif
		}

		if (nMax >= cchMax)
		{
			cchMax = nMax+1;
			pszValue = (wchar_t*)malloc(cchMax*sizeof(*pszValue));
			nMax = pszValue ? GetEnvironmentVariable(pszVarName, szValue, countof(szValue)) : 0;
		}

		if (nMax)
		{
			TODO("Process here ANSI colors TOO! But now it will be 'reentrance'?");
			WriteText(pfnWriteConsoleW, mh_WriteOutput, pszValue, nMax, &cchMax);
		}

		if (pszValue && pszValue != szValue)
			free(pszValue);
		free(pszVarName);
	}
}

// ESC ] 9 ; 9 ; "cwd" ST        Inform ConEmu about shell current working directory
void SrvAnsiImpl::DoSendCWD(LPCWSTR asCmd, ssize_t cchLen)
{
	// We need zero-terminated string
	wchar_t* pszCWD = (wchar_t*)malloc((cchLen + 1)*sizeof(*asCmd));

	if (pszCWD)
	{
		EscCopyCtrlString(pszCWD, asCmd, cchLen);

		// Sends CECMD_STORECURDIR into RConServer
		SendCurrentDirectory(ghConWnd, pszCWD);

		free(pszCWD);
	}
}


bool SrvAnsiImpl::ReportString(LPCWSTR asRet)
{
	if (!asRet || !*asRet)
		return FALSE;
	INPUT_RECORD ir[16] = {};
	int nLen = lstrlen(asRet);
	INPUT_RECORD* pir = (nLen <= (int)countof(ir)) ? ir : (INPUT_RECORD*)calloc(nLen,sizeof(INPUT_RECORD));
	if (!pir)
		return FALSE;

	INPUT_RECORD* p = pir;
	LPCWSTR pc = asRet;
	for (int i = 0; i < nLen; i++, p++, pc++)
	{
		p->EventType = KEY_EVENT;
		p->Event.KeyEvent.bKeyDown = TRUE;
		p->Event.KeyEvent.wRepeatCount = 1;
		p->Event.KeyEvent.uChar.UnicodeChar = *pc;
	}

	DumpKnownEscape(asRet, nLen, de_Report);

	DWORD nWritten = 0;
	HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
	bool bSuccess = WriteConsoleInput(hIn, pir, nLen, &nWritten) && (nWritten == nLen);

	if (pir != ir)
		free(pir);
	return bSuccess;
}

void SrvAnsiImpl::ReportConsoleTitle()
{
	wchar_t sTitle[MAX_PATH*2+6] = L"\x1B]l";
	wchar_t* p = sTitle+3;
	_ASSERTEX(lstrlen(sTitle)==3);

	DWORD nTitle = GetConsoleTitle(sTitle+3, MAX_PATH*2);
	p = sTitle + 3 + std::min<DWORD>(nTitle, MAX_PATH*2);
	*(p++) = L'\x1B';
	*(p++) = L'\\';
	*(p++) = 0;

	ReportString(sTitle);
}

void SrvAnsiImpl::ReportTerminalPixelSize()
{
	// `CSI 4 ; height ; width t`
	wchar_t szReport[64];
	int width = 0, height = 0;
	RECT rcWnd = {};
	CONSOLE_SCREEN_BUFFER_INFO csbi = {};

	if (ghConEmuWndDC && GetClientRect(ghConEmuWndDC, &rcWnd))
	{
		width = RectWidth(rcWnd);
		height = RectHeight(rcWnd);
	}

	if ((width <= 0 || height <= 0) && ghConWnd && GetClientRect(ghConWnd, &rcWnd))
	{
		width = RectWidth(rcWnd);
		height = RectHeight(rcWnd);
	}

	if (width <= 0 || height <= 0)
	{
		_ASSERTE(width > 0 && height > 0);
		// Both DC and RealConsole windows were failed?
		if (GetConsoleScreenBufferInfoCached(GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
		{
			const int defCharWidth = 8, defCharHeight = 14;
			width = (csbi.srWindow.Right - csbi.srWindow.Left + 1) * defCharWidth;
			height = (csbi.srWindow.Bottom - csbi.srWindow.Top + 1) * defCharHeight;
		}
	}

	if (width > 0 && height > 0)
	{
		swprintf_c(szReport, L"\x1B[4;%u;%ut", (uint32_t)height, (uint32_t)width);
		ReportString(szReport);
	}
}

void SrvAnsiImpl::ReportTerminalCharSize(int code)
{
	CONSOLE_SCREEN_BUFFER_INFO csbi = {};
	if (GetConsoleScreenBufferInfoCached(hConsoleOutput, &csbi))
	{
		wchar_t sCurInfo[64];
		msprintf(sCurInfo, countof(sCurInfo),
			L"\x1B[%u;%u;%ut",
			code == 18 ? 8 : 9,
			csbi.srWindow.Bottom-csbi.srWindow.Top+1, csbi.srWindow.Right-csbi.srWindow.Left+1);
		ReportString(sCurInfo);
	}
}

void SrvAnsiImpl::ReportCursorPosition()
{
	CONSOLE_SCREEN_BUFFER_INFO csbi = {};
	if (GetConsoleScreenBufferInfoCached(hConsoleOutput, &csbi))
	{
		wchar_t sCurInfo[32];
		msprintf(sCurInfo, countof(sCurInfo),
			L"\x1B[%u;%uR",
			csbi.dwCursorPosition.Y-csbi.srWindow.Top+1, csbi.dwCursorPosition.X-csbi.srWindow.Left+1);
		ReportString(sCurInfo);
	}
}

bool SrvAnsiImpl::WriteAnsiCodes(LPCWSTR lpBuffer, DWORD nNumberOfCharsToWrite, LPDWORD lpNumberOfCharsWritten)
{
	bool lbRc = TRUE, lbApply = FALSE;
	LPCWSTR lpEnd = (lpBuffer + nNumberOfCharsToWrite);
	AnsiEscCode Code = {};
	wchar_t szPreDump[SrvAnsi::CEAnsi_MaxPrevPart];
	DWORD cchPrevPart;

	//ExtWriteTextParm write = {sizeof(write), ewtf_Current, hConsoleOutput};
	//write.Private = _WriteConsoleW;

	while (lpBuffer < lpEnd)
	{
		LPCWSTR lpStart = NULL, lpNext = NULL; // Required to be NULL-initialized

		// '^' is ESC
		// ^[0;31;47m   $E[31;47m   ^[0m ^[0;1;31;47m  $E[1;31;47m  ^[0m

		cchPrevPart = 0;

		int iEsc = NextEscCode(lpBuffer, lpEnd, szPreDump, cchPrevPart, lpStart, lpNext, Code);

		if (cchPrevPart)
		{
			if (lbApply)
			{
				m_Owner->ReSetDisplayParm(FALSE, TRUE);
				lbApply = FALSE;
			}

			lbRc = WriteText(szPreDump, cchPrevPart, lpNumberOfCharsWritten);
			if (!lbRc)
				goto wrap;
		}

		if (iEsc != 0)
		{
			if (lpStart > lpBuffer)
			{
				_ASSERTEX((lpStart-lpBuffer) < (ssize_t)nNumberOfCharsToWrite);

				if (lbApply)
				{
					m_Owner->ReSetDisplayParm(FALSE, TRUE);
					lbApply = FALSE;
				}

				DWORD nWrite = (DWORD)(lpStart - lpBuffer);
				//lbRc = WriteText(lpBuffer, nWrite, lpNumberOfCharsWritten);
				lbRc = WriteText(lpBuffer, nWrite, lpNumberOfCharsWritten, FALSE);
				if (!lbRc)
					goto wrap;
				//write.Buffer = lpBuffer;
				//write.NumberOfCharsToWrite = nWrite;
				//lbRc = ExtWriteText(&write);
				//if (!lbRc)
				//	goto wrap;
				//else
				//{
				//	if (lpNumberOfCharsWritten)
				//		*lpNumberOfCharsWritten = write.NumberOfCharsWritten;
				//	if (write.ScrolledRowsUp > 0)
				//		gDisplayCursor.StoredCursorPos.Y = std::max(0,((int)gDisplayCursor.StoredCursorPos.Y - (int)write.ScrolledRowsUp));
				//}
			}

			if (iEsc == 1)
			{
				if (Code.Skip)
				{
					DumpUnknownEscape(Code.pszEscStart,Code.nTotalLen);
				}
				else
				{
					switch (Code.Second)
					{
					case L'[':
						{
							lbApply = TRUE;
							WriteAnsiCode_CSI(Code, lbApply);

						} // case L'[':
						break;

					case L']':
						{
							lbApply = TRUE;
							WriteAnsiCode_OSC(Code, lbApply);

						} // case L']':
						break;

					case L'|':
						{
							// vim-xterm-emulation
							lbApply = TRUE;
							WriteAnsiCode_VIM(Code, lbApply);
						} // case L'|':
						break;

					case L'7':
					case L'8':
						//TODO: 7 - Save Cursor and _Attributes_
						//TODO: 8 - Restore Cursor and _Attributes_
						XTermSaveRestoreCursor((Code.Second == L'7'), hConsoleOutput);
						break;
					case L'c':
						// Full reset
						FullReset(hConsoleOutput);
						lbApply = FALSE;
						break;
					case L'M':
						ReverseLF(hConsoleOutput, lbApply);
						break;
					case L'E':
						WriteText(L"\r\n", 2, NULL);
						break;
					case L'D':
						ForwardLF(hConsoleOutput, lbApply);
						break;
					case L'=':
					case L'>':
						// xterm "ESC =" - Application Keypad (DECKPAM)
						// xterm "ESC >" - Normal Keypad (DECKPNM)
						DumpKnownEscape(Code.pszEscStart, Code.nTotalLen, SrvAnsi::de_Ignored);
						break;
					case L'(':
						// xterm G0..G3?
						switch (Code.Action)
						{
						case L'0':
							m_Owner->mCharSet = SrvAnsi::VTCS_DRAWING;
							//DumpKnownEscape(Code.pszEscStart, Code.nTotalLen, de_Comment);
							break;
						case L'B':
							m_Owner->mCharSet = SrvAnsi::VTCS_DEFAULT;
							//DumpKnownEscape(Code.pszEscStart, Code.nTotalLen, de_Comment);
							break;
						default:
							m_Owner->mCharSet = SrvAnsi::VTCS_DEFAULT;
							DumpKnownEscape(Code.pszEscStart, Code.nTotalLen, SrvAnsi::de_Ignored);
						}
						break;

					default:
						DumpUnknownEscape(Code.pszEscStart,Code.nTotalLen);
					}
				}
			}
		}
		else //if (iEsc != 2) // 2 - means "Esc part stored in buffer"
		{
			_ASSERTEX(iEsc == 0);
			if (lpNext > lpBuffer)
			{
				if (lbApply)
				{
					m_Owner->ReSetDisplayParm(FALSE, TRUE);
					lbApply = FALSE;
				}

				DWORD nWrite = (DWORD)(lpNext - lpBuffer);
				//lbRc = WriteText(lpBuffer, nWrite, lpNumberOfCharsWritten);
				lbRc = WriteText(lpBuffer, nWrite, lpNumberOfCharsWritten);
				if (!lbRc)
					goto wrap;
				//write.Buffer = lpBuffer;
				//write.NumberOfCharsToWrite = nWrite;
				//lbRc = ExtWriteText(&write);
				//if (!lbRc)
				//	goto wrap;
				//else
				//{
				//	if (lpNumberOfCharsWritten)
				//		*lpNumberOfCharsWritten = write.NumberOfCharsWritten;
				//	if (write.ScrolledRowsUp > 0)
				//		gDisplayCursor.StoredCursorPos.Y = std::max(0,((int)gDisplayCursor.StoredCursorPos.Y - (int)write.ScrolledRowsUp));
				//}
			}
		}

		if (lpNext > lpBuffer)
		{
			lpBuffer = lpNext;
		}
		else
		{
			_ASSERTEX(lpNext > lpBuffer || lpNext == NULL);
			++lpBuffer;
		}
	}

	if (lbRc && lpNumberOfCharsWritten)
		*lpNumberOfCharsWritten = nNumberOfCharsToWrite;

wrap:
	if (lbApply)
	{
		m_Owner->ReSetDisplayParm(FALSE, TRUE);
		lbApply = FALSE;
	}
	return lbRc;
}

void SrvAnsiImpl::WriteAnsiCode_CSI(AnsiEscCode& Code, bool& lbApply)
{
	/*

CSI ? P m h			DEC Private Mode Set (DECSET)
	P s = 4 7 → Use Alternate Screen Buffer (unless disabled by the titeInhibit resource)
	P s = 1 0 4 7 → Use Alternate Screen Buffer (unless disabled by the titeInhibit resource)
	P s = 1 0 4 8 → Save cursor as in DECSC (unless disabled by the titeInhibit resource)
	P s = 1 0 4 9 → Save cursor as in DECSC and use Alternate Screen Buffer, clearing it first (unless disabled by the titeInhibit resource). This combines the effects of the 1 0 4 7 and 1 0 4 8 modes. Use this with terminfo-based applications rather than the 4 7 mode.

CSI ? P m l			DEC Private Mode Reset (DECRST)
	P s = 4 7 → Use Normal Screen Buffer
	P s = 1 0 4 7 → Use Normal Screen Buffer, clearing screen first if in the Alternate Screen (unless disabled by the titeInhibit resource)
	P s = 1 0 4 8 → Restore cursor as in DECRC (unless disabled by the titeInhibit resource)
	P s = 1 0 4 9 → Use Normal Screen Buffer and restore cursor as in DECRC (unless disabled by the titeInhibit resource). This combines the effects of the 1 0 4 7 and 1 0 4 8 modes. Use this with terminfo-based applications rather than the 4 7 mode.


CSI P s @			Insert P s (Blank) Character(s) (default = 1) (ICH)

	*/
	CONSOLE_SCREEN_BUFFER_INFO csbi = {};

	switch (Code.Action) // case sensitive
	{
	case L's':
		// Save cursor position (can not be nested)
		XTermSaveRestoreCursor(true, hConsoleOutput);
		break;

	case L'u':
		// Restore cursor position
		XTermSaveRestoreCursor(false, hConsoleOutput);
		break;

	case L'H': // Set cursor position (1-based)
	case L'f': // Same as 'H'
	case L'A': // Cursor up by N rows
	case L'B': // Cursor down by N rows
	case L'C': // Cursor right by N cols
	case L'D': // Cursor left by N cols
	case L'E': // Moves cursor to beginning of the line n (default 1) lines down.
	case L'F': // Moves cursor to beginning of the line n (default 1) lines up.
	case L'G': // Moves the cursor to column n.
	case L'd': // Moves the cursor to line n.
		// Change cursor position
		if (GetConsoleScreenBufferInfoCached(hConsoleOutput, &csbi))
		{
			struct {int X,Y;} crNewPos = {csbi.dwCursorPosition.X, csbi.dwCursorPosition.Y};

			switch (Code.Action)
			{
			case L'H':
			case L'f':
				// Set cursor position (1-based)
				crNewPos.Y = csbi.srWindow.Top + ((Code.ArgC > 0 && Code.ArgV[0]) ? (Code.ArgV[0] - 1) : 0);
				crNewPos.X = ((Code.ArgC > 1 && Code.ArgV[1]) ? (Code.ArgV[1] - 1) : 0);
				break;
			case L'A':
				// Cursor up by N rows
				crNewPos.Y -= ((Code.ArgC > 0 && Code.ArgV[0]) ? Code.ArgV[0] : 1);
				break;
			case L'B':
				// Cursor down by N rows
				crNewPos.Y += ((Code.ArgC > 0 && Code.ArgV[0]) ? Code.ArgV[0] : 1);
				break;
			case L'C':
				// Cursor right by N cols
				crNewPos.X += ((Code.ArgC > 0 && Code.ArgV[0]) ? Code.ArgV[0] : 1);
				break;
			case L'D':
				// Cursor left by N cols
				crNewPos.X -= ((Code.ArgC > 0 && Code.ArgV[0]) ? Code.ArgV[0] : 1);
				break;
			case L'E':
				// Moves cursor to beginning of the line n (default 1) lines down.
				crNewPos.Y += ((Code.ArgC > 0 && Code.ArgV[0]) ? Code.ArgV[0] : 1);
				crNewPos.X = 0;
				break;
			case L'F':
				// Moves cursor to beginning of the line n (default 1) lines up.
				crNewPos.Y -= ((Code.ArgC > 0 && Code.ArgV[0]) ? Code.ArgV[0] : 1);
				crNewPos.X = 0;
				break;
			case L'G':
				// Moves the cursor to column n.
				crNewPos.X = ((Code.ArgC > 0) ? (Code.ArgV[0] - 1) : 0);
				break;
			case L'd':
				// Moves the cursor to line n.
				crNewPos.Y = csbi.srWindow.Top + ((Code.ArgC > 0) ? (Code.ArgV[0] - 1) : 0);
				break;
			#ifdef _DEBUG
			default:
				_ASSERTEX(FALSE && "Missed (sub)case value!");
			#endif
			}

			SMALL_RECT clipRgn = csbi.srWindow;
			if (gDisplayOpt.ScrollRegion)
			{
				clipRgn.Top = std::max<int>(0, std::min<int>(gDisplayOpt.ScrollStart, csbi.dwSize.Y-1));
				clipRgn.Bottom = std::max<int>(0, std::min<int>(gDisplayOpt.ScrollEnd, csbi.dwSize.Y-1));
			}
			else if ((Code.Action == 'H')
				&& (((Code.ArgC > 0 && Code.ArgV[0]) ? Code.ArgV[0] : 0) >= csbi.dwSize.Y))
			{
				// #XTERM_256 Allow to put cursor into the legacy true-color area
				clipRgn.Top = 0; clipRgn.Bottom = csbi.dwSize.Y - 1;
			}

			// Check Row
			if (crNewPos.Y < clipRgn.Top)
				crNewPos.Y = clipRgn.Top;
			else if (crNewPos.Y > clipRgn.Bottom)
				crNewPos.Y = clipRgn.Bottom;
			// Check Col
			if (crNewPos.X < clipRgn.Left)
				crNewPos.X = clipRgn.Left;
			else if (crNewPos.X > clipRgn.Right)
				crNewPos.X = clipRgn.Right;
			// Goto
			{
			COORD crNewPosAPI = { crNewPos.X, crNewPos.Y };
			_ASSERTE(crNewPosAPI.X == crNewPos.X && crNewPosAPI.Y == crNewPos.Y);
			F(SetConsoleCursorPosition)(hConsoleOutput, crNewPosAPI);
			}

			if (gbIsVimProcess)
				gbIsVimAnsi = true;
		} // case L'H': case L'f': case 'A': case L'B': case L'C': case L'D':
		break;

	case L'J': // Clears part of the screen
		// Clears the screen and moves the cursor to the home position (line 0, column 0).
		if (GetConsoleScreenBufferInfoCached(hConsoleOutput, &csbi))
		{
			if (lbApply)
			{
				// Apply default color before scrolling!
				// ViM: need to fill whole screen with selected background color, so Apply attributes
				m_Owner->ReSetDisplayParm(FALSE, TRUE);
				lbApply = FALSE;
			}

			int nCmd = (Code.ArgC > 0) ? Code.ArgV[0] : 0;
			bool resetCursor = false;
			COORD cr0 = {};
			int nChars = 0;
			int nScroll = 0;

			switch (nCmd)
			{
			case 0:
				// clear from cursor to end of screen
				cr0 = csbi.dwCursorPosition;
				nChars = (csbi.dwSize.X - csbi.dwCursorPosition.X)
					+ csbi.dwSize.X * (csbi.dwSize.Y - csbi.dwCursorPosition.Y - 1);
				break;
			case 1:
				// clear from cursor to beginning of the screen
				nChars = csbi.dwCursorPosition.X + 1
					+ csbi.dwSize.X * csbi.dwCursorPosition.Y;
				break;
			case 2:
				// clear entire screen and moves cursor to upper left
				nChars = csbi.dwSize.X * csbi.dwSize.Y;
				resetCursor = true;
				break;
			case 3:
				// xterm: clear scrollback buffer entirely
				if (csbi.srWindow.Top > 0)
				{
					nScroll = -csbi.srWindow.Top;
					cr0.X = csbi.dwCursorPosition.X;
					cr0.Y = std::max(0,(csbi.dwCursorPosition.Y-csbi.srWindow.Top));
					resetCursor = true;
				}
				break;
			default:
				DumpUnknownEscape(Code.pszEscStart,Code.nTotalLen);
			}

			if (nChars > 0)
			{
				ExtFillOutputParm fill = {sizeof(fill), efof_Current|efof_Attribute|efof_Character,
					hConsoleOutput, {}, L' ', cr0, (DWORD)nChars};
				ExtFillOutput(&fill);
			}

			if (resetCursor)
			{
				SetConsoleCursorPosition(hConsoleOutput, cr0);
			}

			if (nScroll)
			{
				ScrollScreen(hConsoleOutput, nScroll);
			}

		} // case L'J':
		break;

	case L'b':
		if (!Code.PvtLen)
		{
			int repeat = (Code.ArgC > 0) ? Code.ArgV[0] : 1;
			if (m_LastWrittenChar && repeat > 0)
			{
				CEStr buffer;
				if (wchar_t* ptr = buffer.GetBuffer(repeat))
				{
					for (int i = 0; i < repeat; ++i)
						ptr[i] = m_LastWrittenChar;
					WriteText(ptr, repeat, nullptr);
				}
			}
		}
		else
			DumpUnknownEscape(Code.pszEscStart,Code.nTotalLen);
		break; // case L'b'

	case L'K': // Erases part of the line
		// Clears all characters from the cursor position to the end of the line
		// (including the character at the cursor position).
		if (GetConsoleScreenBufferInfoCached(hConsoleOutput, &csbi))
		{
			if (lbApply)
			{
				// Apply default color before scrolling!
				m_Owner->ReSetDisplayParm(FALSE, TRUE);
				lbApply = FALSE;
			}

			TODO("Need to clear attributes?");
			int nChars = 0;
			int nCmd = (Code.ArgC > 0) ? Code.ArgV[0] : 0;
			COORD cr0 = csbi.dwCursorPosition;

			switch (nCmd)
			{
			case 0: // clear from cursor to the end of the line
				nChars = csbi.dwSize.X - csbi.dwCursorPosition.X;
				break;
			case 1: // clear from cursor to beginning of the line
				cr0.X = 0;
				nChars = csbi.dwCursorPosition.X + 1;
				break;
			case 2: // clear entire line
				cr0.X = 0;
				nChars = csbi.dwSize.X;
				break;
			default:
				DumpUnknownEscape(Code.pszEscStart,Code.nTotalLen);
			}


			if (nChars > 0)
			{
				ExtFillOutputParm fill = {sizeof(fill), efof_Current|efof_Attribute|efof_Character,
					hConsoleOutput, {}, L' ', cr0, (DWORD)nChars };
				ExtFillOutput(&fill);
				//DWORD nWritten = 0;
				//FillConsoleOutputAttribute(hConsoleOutput, GetDefaultTextAttr(), nChars, cr0, &nWritten);
				//FillConsoleOutputCharacter(hConsoleOutput, L' ', nChars, cr0, &nWritten);
			}
		} // case L'K':
		break;

	case L'r':
		//\027[Pt;Pbr
		//
		//Pt is the number of the top line of the scrolling region;
		//Pb is the number of the bottom line of the scrolling region
		// and must be greater than Pt.
		//(The default for Pt is line 1, the default for Pb is the end
		// of the screen)
		//
		if ((Code.ArgC >= 2) && (Code.ArgV[0] >= 1) && (Code.ArgV[1] >= Code.ArgV[0]))
		{
			// Values are 1-based
			SetScrollRegion(true, true, Code.ArgV[0], Code.ArgV[1], hConsoleOutput);
		}
		else
		{
			SetScrollRegion(false);
		}
		break;

	case L'S':
		// Scroll whole page up by n (default 1) lines. New lines are added at the bottom.
		if (lbApply)
		{
			// Apply default color before scrolling!
			m_Owner->ReSetDisplayParm(FALSE, TRUE);
			lbApply = FALSE;
		}
		ScrollScreen(hConsoleOutput, (Code.ArgC > 0 && Code.ArgV[0] > 0) ? -Code.ArgV[0] : -1);
		break;

	case L'L':
		// Insert P s Line(s) (default = 1) (IL).
		LinesInsert(hConsoleOutput, (Code.ArgC > 0 && Code.ArgV[0] > 0) ? Code.ArgV[0] : 1);
		break;
	case L'M':
		// Delete N Line(s) (default = 1) (DL).
		// This is actually "Scroll UP N line(s) inside defined scrolling region"
		LinesDelete(hConsoleOutput, (Code.ArgC > 0 && Code.ArgV[0] > 0) ? Code.ArgV[0] : 1);
		break;

	case L'@':
		// Insert P s (Blank) Character(s) (default = 1) (ICH).
		m_Table->InsertCell((Code.ArgC > 0 && Code.ArgV[0] > 0) ? Code.ArgV[0] : 1);
		break;
	case L'P':
		// Delete P s Character(s) (default = 1) (DCH).
		m_Table->DeleteCell((Code.ArgC > 0 && Code.ArgV[0] > 0) ? Code.ArgV[0] : 1);
		break;

	case L'T':
		// Scroll whole page down by n (default 1) lines. New lines are added at the top.
		if (lbApply)
		{
			// Apply default color before scrolling!
			m_Owner->ReSetDisplayParm(FALSE, TRUE);
		}
		TODO("Define scrolling region");
		ScrollScreen(hConsoleOutput, (Code.ArgC > 0 && Code.ArgV[0] > 0) ? Code.ArgV[0] : 1);
		break;

	case L'h':
	case L'l':
		// Set/ReSet Mode
		if (Code.ArgC > 0)
		{
			//ESC [ 3 h
			//       DECCRM (default off): Display control chars.

			//ESC [ 4 h
			//       DECIM (default off): Set insert mode.

			//ESC [ 20 h
			//       LF/NL (default off): Automatically follow echo of LF, VT or FF with CR.

			//ESC [ ? 1 h
			//	  DECCKM (default off): When set, the cursor keys send an ESC O prefix,
			//	  rather than ESC [.

			//ESC [ ? 3 h
			//	  DECCOLM (default off = 80 columns): 80/132 col mode switch.  The driver
			//	  sources note that this alone does not suffice; some user-mode utility
			//	  such as resizecons(8) has to change the hardware registers on the
			//	  console video card.

			//ESC [ ? 5 h
			//	  DECSCNM (default off): Set reverse-video mode.

			//ESC [ ? 6 h
			//	  DECOM (default off): When set, cursor addressing is relative to the
			//	  upper left corner of the scrolling region.


			//ESC [ ? 8 h
			//	  DECARM (default on): Set keyboard autorepeat on.

			//ESC [ ? 9 h
			//	  X10 Mouse Reporting (default off): Set reporting mode to 1 (or reset to
			//	  0) -- see below.

			//ESC [ ? 1000 h
			//	  X11 Mouse Reporting (default off): Set reporting mode to 2 (or reset to
			//	  0) -- see below.

			//ESC [ ? 7711 h
			//    mimic mintty code, same as "ESC ] 9 ; 12 ST"

			switch (Code.ArgV[0])
			{
			case 1:
				if ((Code.PvtLen == 1) && (Code.Pvt[0] == L'?'))
				{
					gDisplayCursor.CursorKeysApp = (Code.Action == L'h');

					if (gbIsVimProcess)
					{
						TODO("Need to find proper way for activation alternative buffer from ViM?");
						if (Code.Action == L'h')
						{
							StartVimTerm(false);
						}
						else
						{
							StopVimTerm();
						}
					}

					ChangeTermMode(tmc_AppCursorKeys, (Code.Action == L'h'));
				}
				else
					DumpUnknownEscape(Code.pszEscStart, Code.nTotalLen);
				break;
			case 3:
				gDisplayOpt.ShowRawAnsi = (Code.Action == L'h');
				break;
			case 7:
				//ESC [ ? 7 h
				//	  DECAWM (default off): Set autowrap on.  In this mode, a graphic
				//	  character emitted after column 80 (or column 132 of DECCOLM is on)
				//	  forces a wrap to the beginning of the following line first.
				//ESC [ = 7 h
				//    Enables line wrapping
				//ESC [ 7 ; _col_ h
				//    Our extension. _col_ - wrap at column (1-based), default = 80
				if ((gDisplayOpt.WrapWasSet = (Code.Action == L'h')))
				{
					gDisplayOpt.WrapAt = ((Code.ArgC > 1) && (Code.ArgV[1] > 0)) ? Code.ArgV[1] : 80;
				}
				break;
			case 20:
				if (Code.PvtLen == 0)
				{
					gDisplayOpt.AutoLfNl = (Code.Action == L'h');
					DumpKnownEscape(Code.pszEscStart, Code.nTotalLen, SrvAnsi::de_Ignored);
				}
				else
				{
					DumpUnknownEscape(Code.pszEscStart, Code.nTotalLen);
				}
				break;
			//ESC [ ? 12 h
			//	  Start Blinking Cursor (att610)
			case 12:
			//ESC [ ? 25 h
			//	  DECTECM (default on): Make cursor visible.
			case 25:
				if ((Code.PvtLen == 1) && (Code.Pvt[0] == L'?'))
				{
					for (int i = 0; i < Code.ArgC; ++i)
					{
						if (Code.ArgV[i] == 25)
						{
							CONSOLE_CURSOR_INFO ci = {};
							if (GetConsoleCursorInfo(hConsoleOutput, &ci))
							{
								ci.bVisible = (Code.Action == L'h');
								SetConsoleCursorInfo(hConsoleOutput, &ci);
							}
						}
						else
						{
							// DumpKnownEscape(Code.pszEscStart, Code.nTotalLen, SrvAnsi::de_Ignored);
						}
					}
				}
				else
				{
					DumpUnknownEscape(Code.pszEscStart, Code.nTotalLen);
				}
				break;
			case 4:
				if (Code.PvtLen == 0)
				{
					/* h=Insert Mode (IRM), l=Replace Mode (IRM) */
					// Nano posts the `ESC [ 4 l` on start, but do not post `ESC [ 4 h` on exit, that is strange...
					DumpKnownEscape(Code.pszEscStart, Code.nTotalLen, SrvAnsi::de_Ignored); // ignored for now
				}
				else if ((Code.PvtLen == 1) && (Code.Pvt[0] == L'?'))
				{
					/* h=Smooth (slow) scroll, l=Jump (fast) scroll */
					DumpKnownEscape(Code.pszEscStart, Code.nTotalLen, SrvAnsi::de_Ignored); // ignored for now
				}
				else
					DumpUnknownEscape(Code.pszEscStart, Code.nTotalLen);
				break;
			case 9:    /* X10_MOUSE */
			case 1000: /* VT200_MOUSE */
			case 1002: /* BTN_EVENT_MOUSE */
			case 1003: /* ANY_EVENT_MOUSE */
			case 1004: /* FOCUS_EVENT_MOUSE */
			case 1005: /* Xterm's UTF8 encoding for mouse positions */
			case 1006: /* Xterm's CSI-style mouse encoding */
			case 1015: /* Urxvt's CSI-style mouse encoding */
				if ((Code.PvtLen == 1) && (Code.Pvt[0] == L'?'))
				{
					static DWORD LastMode = 0;
					TermMouseMode ModeMask = (Code.ArgV[0] == 9) ? tmm_X10
						: (Code.ArgV[0] == 1000) ? tmm_VT200
						: (Code.ArgV[0] == 1002) ? tmm_BTN
						: (Code.ArgV[0] == 1003) ? tmm_ANY
						: (Code.ArgV[0] == 1004) ? tmm_FOCUS
						: (Code.ArgV[0] == 1005) ? tmm_UTF8
						: (Code.ArgV[0] == 1006) ? tmm_XTERM
						: (Code.ArgV[0] == 1000) ? tmm_URXVT
						: tmm_None;
					DWORD Mode = (Code.Action == L'h')
						? (LastMode | ModeMask)
						: (LastMode & ~ModeMask);
					LastMode = Mode;
					ChangeTermMode(tmc_MouseMode, Mode);
				}
				else
					DumpUnknownEscape(Code.pszEscStart, Code.nTotalLen);
				break;
			case 7786: /* 'V': Mousewheel reporting */
			case 7787: /* 'W': Application mousewheel mode */
				if ((Code.PvtLen == 1) && (Code.Pvt[0] == L'?'))
					DumpKnownEscape(Code.pszEscStart, Code.nTotalLen, SrvAnsi::de_Ignored); // ignored for now
				else
					DumpUnknownEscape(Code.pszEscStart, Code.nTotalLen);
				break;
			case 1034:
				// Interpret "meta" key, sets eighth bit. (enables/disables the eightBitInput resource).
				if ((Code.PvtLen == 1) && (Code.Pvt[0] == L'?'))
					DumpKnownEscape(Code.pszEscStart, Code.nTotalLen, SrvAnsi::de_Ignored);
				else
					DumpUnknownEscape(Code.pszEscStart, Code.nTotalLen);
				break;
			case 47:   /* alternate screen */
			case 1047: /* alternate screen */
			case 1049: /* cursor & alternate screen */
				// xmux/screen: Alternate screen
				if ((Code.PvtLen == 1) && (Code.Pvt[0] == L'?'))
				{
					if (lbApply)
					{
						m_Owner->ReSetDisplayParm(FALSE, TRUE);
						lbApply = FALSE;
					}

					// \e[?1049h: save cursor pos
					if ((Code.ArgV[0] == 1049) && (Code.Action == L'h'))
						XTermSaveRestoreCursor(true, hConsoleOutput);
					// h: switch to alternative buffer without backscroll
					// l: restore saved scrollback buffer
					XTermAltBuffer((Code.Action == L'h'));
					// \e[?1049l - restore cursor pos
					if ((Code.ArgV[0] == 1049) && (Code.Action == L'l'))
						XTermSaveRestoreCursor(false, hConsoleOutput);
				}
				else
				{
					DumpUnknownEscape(Code.pszEscStart, Code.nTotalLen);
				}
				break;
			case 1048: /* save/restore cursor */
				if ((Code.PvtLen == 1) && (Code.Pvt[0] == L'?'))
					XTermSaveRestoreCursor((Code.Action == L'h'), hConsoleOutput);
				else
					DumpUnknownEscape(Code.pszEscStart, Code.nTotalLen);
				break;
			case 2004: /* bracketed paste */
				/* All "pasted" text will be wrapped in `\e[200~ ... \e[201~` */
				if ((Code.PvtLen == 1) && (Code.Pvt[0] == L'?'))
					ChangeTermMode(tmc_BracketedPaste, (Code.Action == L'h'));
				else
					DumpUnknownEscape(Code.pszEscStart, Code.nTotalLen);
				break;
			case 7711:
				if ((Code.Action == L'h') && (Code.PvtLen == 1) && (Code.Pvt[0] == L'?'))
				{
					StorePromptBegin();
				}
				else
				{
					DumpUnknownEscape(Code.pszEscStart, Code.nTotalLen);
				}
				break;
			default:
				DumpUnknownEscape(Code.pszEscStart,Code.nTotalLen);
			}

			//switch (Code.ArgV[0])
			//{
			//case 0: case 1:
			//	// 40x25
			//	if ((gDisplayOpt.WrapWasSet = (Code.Action == L'h')))
			//	{
			//		gDisplayOpt.WrapAt = 40;
			//	}
			//	break;
			//case 2: case 3:
			//	// 80x25
			//	if ((gDisplayOpt.WrapWasSet = (Code.Action == L'h')))
			//	{
			//		gDisplayOpt.WrapAt = 80;
			//	}
			//	break;
			//case 7:
			//	{
			//		DWORD Mode = 0;
			//		GetConsoleMode(hConsoleOutput, &Mode);
			//		if (Code.Action == L'h')
			//			Mode |= ENABLE_WRAP_AT_EOL_OUTPUT;
			//		else
			//			Mode &= ~ENABLE_WRAP_AT_EOL_OUTPUT;
			//		SetConsoleMode(hConsoleOutput, Mode);
			//	} // enable/disable line wrapping
			//	break;
			//}
		}
		else
		{
			DumpUnknownEscape(Code.pszEscStart,Code.nTotalLen);
		}
		break; // case L'h': case L'l':

	case L'n':
		if (Code.ArgC > 0)
		{
			switch (*Code.ArgV)
			{
			case 5:
				//ESC [ 5 n
				//      Device status report (DSR): Answer is ESC [ 0 n (Terminal OK).
				//
				ReportString(L"\x1B[0n");
				break;
			case 6:
				//ESC [ 6 n
				//      Cursor position report (CPR): Answer is ESC [ y ; x R, where x,y is the
				//      cursor location.
				ReportCursorPosition(hConsoleOutput);
				break;
			default:
				DumpUnknownEscape(Code.pszEscStart,Code.nTotalLen);
			}
		}
		else
		{
			DumpUnknownEscape(Code.pszEscStart,Code.nTotalLen);
		}
		break;

	case L'm':
		if (Code.PvtLen > 0)
		{
			DumpKnownEscape(Code.pszEscStart, Code.nTotalLen, SrvAnsi::de_Ignored);
		}
		// Set display mode (colors, fonts, etc.)
		else if (!Code.ArgC)
		{
			m_Owner->ReSetDisplayParm(TRUE, FALSE);
		}
		else
		{
			for (int i = 0; i < Code.ArgC; i++)
			{
				switch (Code.ArgV[i])
				{
				case 0:
					m_Owner->ReSetDisplayParm(TRUE, FALSE);
					break;
				case 1:
					// Bold
					m_Owner->gDisplayParm.setBrightOrBold(TRUE);
					break;
				case 2:
					// Faint, decreased intensity (ISO 6429)
				case 22:
					// Normal (neither bold nor faint).
					m_Owner->gDisplayParm.setBrightOrBold(FALSE);
					break;
				case 3:
					// Italic
					m_Owner->gDisplayParm.setItalic(TRUE);
					break;
				case 23:
					// Not italic
					m_Owner->gDisplayParm.setItalic(FALSE);
					break;
				case 5: // #TODO ANSI Slow Blink (less than 150 per minute)
				case 6: // #TODO ANSI Rapid Blink (150+ per minute)
				case 25: // #TODO ANSI Blink Off
					DumpKnownEscape(Code.pszEscStart,Code.nTotalLen,SrvAnsi::de_Ignored);
					break;
				case 4: // Underlined
					m_Owner->gDisplayParm.setUnderline(TRUE);
					break;
				case 24:
					// Not underlined
					m_Owner->gDisplayParm.setUnderline(FALSE);
					break;
				case 7:
					// Reverse video
					m_Owner->gDisplayParm.setInverse(TRUE);
					break;
				case 27:
					// Positive (not inverse)
					m_Owner->gDisplayParm.setInverse(FALSE);
					break;
				case 30: case 31: case 32: case 33: case 34: case 35: case 36: case 37:
					m_Owner->gDisplayParm.setTextColor(Code.ArgV[i] - 30);
					m_Owner->gDisplayParm.setBrightFore(FALSE);
					m_Owner->gDisplayParm.setText256(FALSE);
					break;
				case 38:
					// xterm-256 colors
					// ESC [ 38 ; 5 ; I m -- set foreground to I (0..255) color from xterm palette
					if (((i+2) < Code.ArgC) && (Code.ArgV[i+1] == 5))
					{
						m_Owner->gDisplayParm.setTextColor(Code.ArgV[i+2] & 0xFF);
						m_Owner->gDisplayParm.setText256(1);
						i += 2;
					}
					// xterm-256 colors
					// ESC [ 38 ; 2 ; R ; G ; B m -- set foreground to RGB(R,G,B) 24-bit color
					else if (((i+4) < Code.ArgC) && (Code.ArgV[i+1] == 2))
					{
						m_Owner->gDisplayParm.setTextColor(RGB((Code.ArgV[i+2] & 0xFF),(Code.ArgV[i+3] & 0xFF),(Code.ArgV[i+4] & 0xFF)));
						m_Owner->gDisplayParm.setText256(2);
						i += 4;
					}
					break;
				case 39:
					// Reset
					m_Owner->gDisplayParm.setTextColor(CONFORECOLOR(GetDefaultTextAttr()));
					m_Owner->gDisplayParm.setBrightFore(FALSE);
					m_Owner->gDisplayParm.setText256(FALSE);
					break;
				case 40: case 41: case 42: case 43: case 44: case 45: case 46: case 47:
					m_Owner->gDisplayParm.setBackColor(Code.ArgV[i] - 40);
					m_Owner->gDisplayParm.setBrightBack(FALSE);
					m_Owner->gDisplayParm.setBack256(FALSE);
					break;
				case 48:
					// xterm-256 colors
					// ESC [ 48 ; 5 ; I m -- set background to I (0..255) color from xterm palette
					if (((i+2) < Code.ArgC) && (Code.ArgV[i+1] == 5))
					{
						m_Owner->gDisplayParm.setBackColor(Code.ArgV[i+2] & 0xFF);
						m_Owner->gDisplayParm.setBack256(1);
						i += 2;
					}
					// xterm-256 colors
					// ESC [ 48 ; 2 ; R ; G ; B m -- set background to RGB(R,G,B) 24-bit color
					else if (((i+4) < Code.ArgC) && (Code.ArgV[i+1] == 2))
					{
						m_Owner->gDisplayParm.setBackColor(RGB((Code.ArgV[i+2] & 0xFF),(Code.ArgV[i+3] & 0xFF),(Code.ArgV[i+4] & 0xFF)));
						m_Owner->gDisplayParm.setBack256(2);
						i += 4;
					}
					break;
				case 49:
					// Reset
					m_Owner->gDisplayParm.setBackColor(CONBACKCOLOR(GetDefaultTextAttr()));
					m_Owner->gDisplayParm.setBrightBack(FALSE);
					m_Owner->gDisplayParm.setBack256(FALSE);
					break;
				case 90: case 91: case 92: case 93: case 94: case 95: case 96: case 97:
					m_Owner->gDisplayParm.setTextColor((Code.ArgV[i] - 90) | 0x8);
					m_Owner->gDisplayParm.setText256(FALSE);
					m_Owner->gDisplayParm.setBrightFore(TRUE);
					break;
				case 100: case 101: case 102: case 103: case 104: case 105: case 106: case 107:
					m_Owner->gDisplayParm.setBackColor((Code.ArgV[i] - 100) | 0x8);
					m_Owner->gDisplayParm.setBack256(FALSE);
					m_Owner->gDisplayParm.setBrightBack(TRUE);
					break;
				case 10:
					// Something strange and unknown... (received from ssh)
					DumpKnownEscape(Code.pszEscStart, Code.nTotalLen, SrvAnsi::de_Ignored);
					break;
				case 312:
				case 315:
				case 414:
				case 3130:
					// Something strange and unknown... (received from vim on WSL)
					DumpKnownEscape(Code.pszEscStart, Code.nTotalLen, SrvAnsi::de_Ignored);
					break;
				default:
					DumpUnknownEscape(Code.pszEscStart,Code.nTotalLen);
				}
			}
		}
		break; // "[...m"

	case L'q':
		if ((Code.PvtLen == 1) && (Code.Pvt[0] == L' '))
		{
			/*
			CSI Ps SP q
				Set cursor style (DECSCUSR, VT520).
					Ps = 0  -> ConEmu's default.
					Ps = 1  -> blinking block.
					Ps = 2  -> steady block.
					Ps = 3  -> blinking underline.
					Ps = 4  -> steady underline.
					Ps = 5  -> blinking bar (xterm).
					Ps = 6  -> steady bar (xterm).
			*/
			DWORD nStyle = ((Code.ArgC == 0) || (Code.ArgV[0] < 0) || (Code.ArgV[0] > 6))
				? 0 : Code.ArgV[0];
			CONSOLE_CURSOR_INFO ci = {};
			if (GetConsoleCursorInfo(hConsoleOutput, &ci))
			{
				// We can't implement all possible styles in RealConsole,
				// but we can use "Block/Underline" shapes...
				ci.dwSize = (nStyle == 1 || nStyle == 2) ? 100 : 15;
				SetConsoleCursorInfo(hConsoleOutput, &ci);
			}
			ChangeTermMode(tmc_CursorShape, nStyle);
		}
		else
		{
			DumpUnknownEscape(Code.pszEscStart,Code.nTotalLen);
		}
		break; // "[...q"

	case L't':
		if (Code.ArgC > 0 && Code.ArgC <= 3)
		{
			wchar_t sCurInfo[32];
			for (int i = 0; i < Code.ArgC; i++)
			{
				switch (Code.ArgV[i])
				{
				case 8:
					// `ESC [ 8 ; height ; width t` --> Resize the text area to [height;width] in characters.
					{
						int height = -1, width = -1;
						if (i < Code.ArgC)
							height = Code.ArgV[++i];
						if (i < Code.ArgC)
							width = Code.ArgV[++i];
						DumpKnownEscape(Code.pszEscStart, Code.nTotalLen, SrvAnsi::de_Ignored);
					}
					break;
				case 14:
					// `ESC [ 1 4 t` --> Reports terminal window size in pixels as `CSI 4 ; height ; width t`.
					ReportTerminalPixelSize();
					break;
				case 18:
				case 19:
					// `ESC [ 1 8 t` --> Report the size of the text area in characters as `CSI 8 ; height ; width t`
					// `ESC [ 1 9 t` --> Report the size of the screen in characters as `CSI 9 ; height ; width t`
					ReportTerminalCharSize(hConsoleOutput, Code.ArgV[i]);
					break;
				case 21:
					// `ESC [ 2 1 t` --> Report terminal window title as `OSC l title ST`
					ReportConsoleTitle();
					break;
				default:
					TODO("ANSI: xterm window manipulation");
					//Window manipulation (from dtterm, as well as extensions). These controls may be disabled using the allowWindowOps resource. Valid values for the first (and any additional parameters) are:
					// 1 --> De-iconify window.
					// 2 --> Iconify window.
					// 3 ; x ; y --> Move window to [x, y].
					// 4 ; height ; width --> Resize the xterm window to height and width in pixels.
					// 5 --> Raise the xterm window to the front of the stacking order.
					// 6 --> Lower the xterm window to the bottom of the stacking order.
					// 7 --> Refresh the xterm window.
					// 8 ; height ; width --> Resize the text area to [height;width] in characters.
					// 9 ; 0 --> Restore maximized window.
					// 9 ; 1 --> Maximize window (i.e., resize to screen size).
					// 1 1 --> Report xterm window state. If the xterm window is open (non-iconified), it returns CSI 1 t . If the xterm window is iconified, it returns CSI 2 t .
					// 1 3 --> Report xterm window position as CSI 3 ; x; y t
					// 1 4 --> Report xterm window in pixels as CSI 4 ; height ; width t
					// 2 0 --> Report xterm window�s icon label as OSC L label ST
					// >= 2 4 --> Resize to P s lines (DECSLPP)
					DumpUnknownEscape(Code.pszEscStart,Code.nTotalLen);
				}
			}
		}
		else
		{
			DumpUnknownEscape(Code.pszEscStart,Code.nTotalLen);
		}
		break;

	case L'c':
		// echo -e "\e[>c"
		if ((Code.PvtLen == 1) && (Code.Pvt[0] == L'>')
			&& ((Code.ArgC < 1) || (Code.ArgV[0] == 0)))
		{
			// P s = 0 or omitted -> request the terminal's identification code.
			wchar_t szVerInfo[64];
			// this will be "ESC > 67 ; build ; 0 c"
			// 67 is ASCII code of 'C' (ConEmu, yeah)
			// Other terminals report examples: MinTTY -> 77, rxvt -> 82, screen -> 83
			// msprintf(szVerInfo, countof(szVerInfo), L"\x1B>%u;%u;0c", (int)'C', MVV_1*10000+MVV_2*100+MVV_3);
			// Emulate xterm version 136?
			wcscpy_c(szVerInfo, L"\x1B[>0;136;0c");
			ReportString(szVerInfo);
		}
		// echo -e "\e[c"
		else if ((Code.ArgC < 1) || (Code.ArgV[0] == 0))
		{
			// Report "VT100 with Advanced Video Option"
			ReportString(L"\x1B[?1;2c");
		}
		else
		{
			DumpUnknownEscape(Code.pszEscStart,Code.nTotalLen);
		}
		break;

	case L'X':
		// CSI P s X:  Erase P s Character(s) (default = 1) (ECH)
		if (GetConsoleScreenBufferInfoCached(hConsoleOutput, &csbi))
		{
			if (lbApply)
			{
				// Apply default color before scrolling!
				m_Owner->ReSetDisplayParm(FALSE, TRUE);
				lbApply = FALSE;
			}

			int nCount = (Code.ArgC > 0) ? Code.ArgV[0] : 1;
			int nScreenLeft = csbi.dwSize.X - csbi.dwCursorPosition.X - 1 + (csbi.dwSize.X * (csbi.dwSize.Y - csbi.dwCursorPosition.Y - 1));
			int nChars = std::min(nCount,nScreenLeft);
			COORD cr0 = csbi.dwCursorPosition;

			if (nChars > 0)
			{
				ExtFillOutputParm fill = {sizeof(fill), efof_Current|efof_Attribute|efof_Character,
					hConsoleOutput, {}, L' ', cr0, (DWORD)nChars };
				ExtFillOutput(&fill);
			}
		} // case L'X':
		break;

	default:
		DumpUnknownEscape(Code.pszEscStart,Code.nTotalLen);
	} // switch (Code.Action)
}

void SrvAnsiImpl::WriteAnsiCode_OSC(AnsiEscCode& Code, bool& lbApply)
{
	if (!Code.ArgSZ)
	{
		DumpUnknownEscape(Code.pszEscStart,Code.nTotalLen);
		return;
	}

	// Finalizing (ST) with "\x1B\\" or "\x07"
	//ESC ] 0 ; txt ST        Set icon name and window title to txt.
	//ESC ] 1 ; txt ST        Set icon name to txt.
	//ESC ] 2 ; txt ST        Set window title to txt.
	//ESC ] 4 ; num; txt ST   Set ANSI color num to txt.
	//ESC ] 9 ... ST          ConEmu specific
	//ESC ] 10 ; txt ST       Set dynamic text color to txt.
	//ESC ] 4 6 ; name ST     Change log file to name (normally disabled
	//					      by a compile-time option)
	//ESC ] 5 0 ; fn ST       Set font to fn.

	switch (*Code.ArgSZ)
	{
	case L'0':
	case L'1':
	case L'2':
		if (Code.ArgSZ[1] == L';' && Code.ArgSZ[2])
		{
			wchar_t* pszNewTitle = (wchar_t*)malloc(sizeof(wchar_t)*(Code.cchArgSZ));
			if (pszNewTitle)
			{
				EscCopyCtrlString(pszNewTitle, Code.ArgSZ+2, Code.cchArgSZ-2);
				SetConsoleTitle(*pszNewTitle ? pszNewTitle : gsInitConTitle);
				free(pszNewTitle);
			}
		}
		break;

	case L'4':
		// TODO: the following is suggestion for exact palette colors
		// TODO: but we are using standard xterm palette or truecolor 24bit palette
		_ASSERTEX(Code.ArgSZ[1] == L';');
		break;

	case L'9':
		// ConEmu specific
		// ESC ] 9 ; 1 ; ms ST           Sleep. ms - milliseconds
		// ESC ] 9 ; 2 ; "txt" ST        Show GUI MessageBox ( txt ) for dubug purposes
		// ESC ] 9 ; 3 ; "txt" ST        Set TAB text
		// ESC ] 9 ; 4 ; st ; pr ST      When _st_ is 0: remove progress. When _st_ is 1: set progress value to _pr_ (number, 0-100). When _st_ is 2: set error state in progress on Windows 7 taskbar
		// ESC ] 9 ; 5 ST                Wait for ENTER/SPACE/ESC. Set EnvVar "ConEmuWaitKey" to ENTER/SPACE/ESC on exit.
		// ESC ] 9 ; 6 ; "txt" ST        Execute GuiMacro. Set EnvVar "ConEmuMacroResult" on exit.
		// ESC ] 9 ; 7 ; "cmd" ST        Run some process with arguments
		// ESC ] 9 ; 8 ; "env" ST        Output value of environment variable
		// ESC ] 9 ; 9 ; "cwd" ST        Inform ConEmu about shell current working directory
		// ESC ] 9 ; 10 ST               Request xterm keyboard emulation
		// ESC ] 9 ; 11; "*txt*" ST      Just a ‘comment’, skip it.
		// ESC ] 9 ; 12 ST               Let ConEmu treat current cursor position as prompt start. Useful with `PS1`.
		if (Code.ArgSZ[1] == L';')
		{
			if (Code.ArgSZ[2] == L'1')
			{
				if (Code.ArgSZ[3] == L';')
				{
					// ESC ] 9 ; 1 ; ms ST
					DoSleep(Code.ArgSZ+4);
				}
				else if (Code.ArgC >= 2 && Code.ArgV[1] == 10)
				{
					// ESC ] 9 ; 10 ST
					if (!gbWasXTermOutput && (Code.ArgC == 2 || Code.ArgV[2] != 0))
						SrvAnsiImpl::StartXTermMode(true);
					else if (Code.ArgC >= 3 || Code.ArgV[2] == 0)
						SrvAnsiImpl::StartXTermMode(false);
				}
				else if (Code.ArgSZ[3] == L'1' && Code.ArgSZ[4] == L';')
				{
					// ESC ] 9 ; 11; "*txt*" ST - Just a ‘comment’, skip it.
					DumpKnownEscape(Code.ArgSZ+5, lstrlen(Code.ArgSZ+5), de_Comment);
				}
				else if (Code.ArgSZ[3] == L'2')
				{
					// ESC ] 9 ; 12 ST
					StorePromptBegin();
				}
			}
			else if (Code.ArgSZ[2] == L'2' && Code.ArgSZ[3] == L';')
			{
				DoMessage(Code.ArgSZ+4, Code.cchArgSZ - 4);
			}
			else if (Code.ArgSZ[2] == L'3' && Code.ArgSZ[3] == L';')
			{
				CESERVER_REQ* pIn = ExecuteNewCmd(CECMD_SETTABTITLE, sizeof(CESERVER_REQ_HDR)+sizeof(wchar_t)*(Code.cchArgSZ));
				if (pIn)
				{
					EscCopyCtrlString((wchar_t*)pIn->wData, Code.ArgSZ+4, Code.cchArgSZ-4);
					CESERVER_REQ* pOut = ExecuteGuiCmd(ghConWnd, pIn, ghConWnd);
					ExecuteFreeResult(pIn);
					ExecuteFreeResult(pOut);
				}
			}
			else if (Code.ArgSZ[2] == L'4')
			{
				WORD st = 0, pr = 0;
				LPCWSTR pszName = NULL;
				if (Code.ArgSZ[3] == L';')
				{
					switch (Code.ArgSZ[4])
					{
					case L'0':
						break;
					case L'1': // Normal
					case L'2': // Error
						st = Code.ArgSZ[4] - L'0';
						if (Code.ArgSZ[5] == L';')
						{
							LPCWSTR pszValue = Code.ArgSZ + 6;
							pr = NextNumber(pszValue);
						}
						break;
					case L'3':
						st = 3; // Indeterminate
						break;
					case L'4':
					case L'5':
						st = Code.ArgSZ[4] - L'0';
						pszName = (Code.ArgSZ[5] == L';') ? (Code.ArgSZ + 6) : NULL;
						break;
					}
				}
				GuiSetProgress(st,pr,pszName);
			}
			else if (Code.ArgSZ[2] == L'5')
			{
				//int s = 0;
				//if (Code.ArgSZ[3] == L';')
				//	s = NextNumber(Code.ArgSZ+4);
				bool bSucceeded = FALSE;
				DWORD nRead = 0;
				INPUT_RECORD r = {};
				HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
				//DWORD nStartTick = GetTickCount();
				while ((bSucceeded = ReadConsoleInput(hIn, &r, 1, &nRead)) && nRead)
				{
					if ((r.EventType == KEY_EVENT) && r.Event.KeyEvent.bKeyDown)
					{
						if ((r.Event.KeyEvent.wVirtualKeyCode == VK_RETURN)
							|| (r.Event.KeyEvent.wVirtualKeyCode == VK_SPACE)
							|| (r.Event.KeyEvent.wVirtualKeyCode == VK_ESCAPE))
						{
							break;
						}
					}
				}
				if (bSucceeded && ((r.Event.KeyEvent.wVirtualKeyCode == VK_RETURN)
							|| (r.Event.KeyEvent.wVirtualKeyCode == VK_SPACE)
							|| (r.Event.KeyEvent.wVirtualKeyCode == VK_ESCAPE)))
				{
					SetEnvironmentVariable(ENV_CONEMU_WAITKEY_W,
						(r.Event.KeyEvent.wVirtualKeyCode == VK_RETURN) ? L"RETURN" :
						(r.Event.KeyEvent.wVirtualKeyCode == VK_SPACE)  ? L"SPACE" :
						(r.Event.KeyEvent.wVirtualKeyCode == VK_ESCAPE) ? L"ESC" :
						L"???");
				}
				else
				{
					SetEnvironmentVariable(ENV_CONEMU_WAITKEY_W, L"");
				}
			}
			else if (Code.ArgSZ[2] == L'6' && Code.ArgSZ[3] == L';')
			{
				DoGuiMacro(Code.ArgSZ+4, Code.cchArgSZ - 4);
			}
			else if (Code.ArgSZ[2] == L'7' && Code.ArgSZ[3] == L';')
			{
				DoProcess(Code.ArgSZ+4, Code.cchArgSZ - 4);
			}
			else if (Code.ArgSZ[2] == L'8' && Code.ArgSZ[3] == L';')
			{
				if (lbApply)
				{
					m_Owner->ReSetDisplayParm(FALSE, TRUE);
					lbApply = FALSE;
				}
				DoPrintEnv(Code.ArgSZ+4, Code.cchArgSZ - 4);
			}
			else if (Code.ArgSZ[2] == L'9' && Code.ArgSZ[3] == L';')
			{
				DoSendCWD(Code.ArgSZ+4, Code.cchArgSZ - 4);
			}
		}
		break;

	default:
		DumpUnknownEscape(Code.pszEscStart,Code.nTotalLen);
	}
}

void SrvAnsiImpl::WriteAnsiCode_VIM(AnsiEscCode& Code, bool& lbApply)
{
	if (!gbWasXTermOutput && !gnWriteProcessed)
	{
		SrvAnsiImpl::StartXTermMode(true);
	}

	switch (Code.Action)
	{
	case L'm':
		// Set xterm display modes (colors, fonts, etc.)
		if (!Code.ArgC)
		{
			//m_Owner->ReSetDisplayParm(TRUE, FALSE);
			DumpUnknownEscape(Code.pszEscStart,Code.nTotalLen);
		}
		else
		{
			for (int i = 0; i < Code.ArgC; i++)
			{
				switch (Code.ArgV[i])
				{
				case 7:
					m_Owner->gDisplayParm.setBrightOrBold(FALSE);
					m_Owner->gDisplayParm.setItalic(FALSE);
					m_Owner->gDisplayParm.setUnderline(FALSE);
					m_Owner->gDisplayParm.setBrightFore(FALSE);
					m_Owner->gDisplayParm.setBrightBack(FALSE);
					m_Owner->gDisplayParm.setInverse(FALSE);
					break;
				case 15:
					m_Owner->gDisplayParm.setBrightOrBold(TRUE);
					break;
				case 112:
					m_Owner->gDisplayParm.setInverse(TRUE);
					break;
				case 143:
					// What is this?
					break;
				default:
					DumpUnknownEscape(Code.pszEscStart,Code.nTotalLen);
				}
			}
		}
		break; // "|...m"
	}
}

void SrvAnsiImpl::SetScrollRegion(bool bRegion, int nStart, int nEnd)
{
	m_Table->SetScrollRegion(bRegion, nStart, nEnd);
}

void SrvAnsiImpl::XTermSaveRestoreCursor(bool bSaveCursor)
{
	if (bSaveCursor)
	{
		m_Owner->gDisplayCursor.bCursorPosStored = true;
		m_Owner->gDisplayCursor.StoredCursorPos = m_Table->GetCursor();
	}
	else
	{
		// Restore cursor position
		m_Table->SetCursor(m_Owner->gDisplayCursor.bCursorPosStored ? m_Owner->gDisplayCursor.StoredCursorPos : condata::Coord{});
	}
}

/// Change current buffer
/// Alternative buffer in XTerm is used to "fullscreen"
/// applications like Vim. There is no scrolling and this
/// mode is used to save current backscroll contents and
/// restore it when application exits
void SrvAnsiImpl::XTermAltBuffer(bool bSetAltBuffer/*, condata::TablePtr& table*/)
{
	if (bSetAltBuffer)
	{
		// Once!
		if (!m_Owner->m_UsePrimary)
			return;

		const auto attr = m_Table->GetAttr();
		m_Table = m_Owner->GetTable(SrvAnsi::GetTableEnum::alternative);
		m_Table->Reset(attr);

		// #condata Save gDisplayCursor.StoredCursorPos separately by primary/alternative

	}
	else
	{
		if (m_Owner->m_UsePrimary)
			return;

		m_Table->Reset({});

		m_Table = m_Owner->GetTable(SrvAnsi::GetTableEnum::primary);

		// #condata Save gDisplayCursor.StoredCursorPos separately by primary/alternative

	}
}


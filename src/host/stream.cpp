/********************************************************
*                                                       *
*   Copyright (C) Microsoft. All rights reserved.       *
*                                                       *
********************************************************/

#include "precomp.h"

#include "_stream.h"
#include "stream.h"

#include "dbcs.h"
#include "handle.h"
#include "misc.h"
#include "readDataRaw.hpp"

#include "ApiRoutines.h"

#include "..\interactivity\inc\ServiceLocator.hpp"

#pragma hdrstop

#define LINE_INPUT_BUFFER_SIZE (256 * sizeof(WCHAR))

#define IS_JPN_1BYTE_KATAKANA(c)   ((c) >= 0xa1 && (c) <= 0xdf)

// Routine Description:
// - This routine is used in stream input.  It gets input and filters it for unicode characters.
// Arguments:
// - pInputBuffer - The InputBuffer to read from
// - pwchOut - On a successful read, the char data read
// - Wait - true if a waited read should be performed
// - pCommandLineEditingKeys - if present, arrow keys will be
// returned. on output, if true, pwchOut contains virtual key code for
// arrow key.
// - pCommandLinePopupKeys - if present, arrow keys will be
// returned. on output, if true, pwchOut contains virtual key code for
// arrow key.
// Return Value:
// - STATUS_SUCCESS on success or a relevant error code on failure.
NTSTATUS GetChar(_Inout_ InputBuffer* const pInputBuffer,
                 _Out_ wchar_t* const pwchOut,
                 _In_ const bool Wait,
                 _Out_opt_ bool* const pCommandLineEditingKeys,
                 _Out_opt_ bool* const pCommandLinePopupKeys,
                 _Out_opt_ DWORD* const pdwKeyState)
{
    if (nullptr != pCommandLineEditingKeys)
    {
        *pCommandLineEditingKeys = false;
    }

    if (nullptr != pCommandLinePopupKeys)
    {
        *pCommandLinePopupKeys = false;
    }

    if (nullptr != pdwKeyState)
    {
        *pdwKeyState = 0;
    }

    NTSTATUS Status;
    for (;;)
    {
        std::unique_ptr<IInputEvent> inputEvent;
        Status = pInputBuffer->Read(inputEvent,
                                    false, // peek
                                    Wait,
                                    true); // unicode

        if (!NT_SUCCESS(Status))
        {
            return Status;
        }

        if (inputEvent.get() == nullptr)
        {
            assert(!Wait);
            return STATUS_UNSUCCESSFUL;
        }

        if (inputEvent->EventType() == InputEventType::KeyEvent)
        {
            std::unique_ptr<KeyEvent> keyEvent = std::unique_ptr<KeyEvent>(static_cast<KeyEvent*>(inputEvent.release()));

            bool commandLineEditKey = false;
            if (pCommandLineEditingKeys)
            {
                commandLineEditKey = keyEvent->IsCommandLineEditingKey();
            }
            else if (pCommandLinePopupKeys)
            {
                commandLineEditKey = keyEvent->IsCommandLinePopupKey();
            }

            if ((pCommandLineEditingKeys || pCommandLinePopupKeys) &&
                ServiceLocator::LocateGlobals()->getConsoleInformation()->GetExtendedEditKey() &&
                keyEvent->GetKeySubst())
            {
                keyEvent->ParseEditKeyInfo();
            }

            if (pdwKeyState)
            {
                *pdwKeyState = keyEvent->_activeModifierKeys;
            }

            if (keyEvent->_charData != 0 && !commandLineEditKey)
            {
                // chars that are generated using alt + numpad
                if (!keyEvent->_keyDown && keyEvent->_virtualKeyCode == VK_MENU)
                {
                    if (IsFlagSet(keyEvent->_activeModifierKeys, ALTNUMPAD_BIT))
                    {
                        if (HIBYTE(keyEvent->_charData))
                        {
                            char chT[2] = {
                                static_cast<char>(HIBYTE(keyEvent->_charData)),
                                static_cast<char>(LOBYTE(keyEvent->_charData)),
                            };
                            *pwchOut = CharToWchar(chT, 2);
                        }
                        else
                        {
                            // Because USER doesn't know our codepage,
                            // it gives us the raw OEM char and we
                            // convert it to a Unicode character.
                            char chT = LOBYTE(keyEvent->_charData);
                            *pwchOut = CharToWchar(&chT, 1);
                        }
                    }
                    else
                    {
                        *pwchOut = keyEvent->_charData;
                    }
                    return STATUS_SUCCESS;
                }
                // Ignore Escape and Newline chars
                else if (keyEvent->_keyDown &&
                         (IsFlagSet(pInputBuffer->InputMode, ENABLE_VIRTUAL_TERMINAL_INPUT) ||
                          (keyEvent->_virtualKeyCode != VK_ESCAPE &&
                           keyEvent->_charData != UNICODE_LINEFEED)))
                {
                    *pwchOut = keyEvent->_charData;
                    return STATUS_SUCCESS;
                }
            }

            if (keyEvent->_keyDown)
            {
                if (pCommandLineEditingKeys && commandLineEditKey)
                {
                    *pCommandLineEditingKeys = true;
                    *pwchOut = static_cast<wchar_t>(keyEvent->_virtualKeyCode);
                }
                else if (pCommandLinePopupKeys && commandLineEditKey)
                {
                    *pCommandLinePopupKeys = true;
                    *pwchOut = static_cast<char>(keyEvent->_virtualKeyCode);
                }
                else
                {
                    const short zeroVkeyData = ServiceLocator::LocateInputServices()->VkKeyScanW(0);
                    const byte zeroVKey = LOBYTE(zeroVkeyData);
                    const byte zeroControlKeyState = HIBYTE(zeroVkeyData);
                    // Convert real Windows NT modifier bit into bizarre Console bits
                    const DWORD consoleModifierTranslator[] =
                    {
                        0,
                        SHIFT_PRESSED,
                        CTRL_PRESSED,
                        SHIFT_PRESSED | CTRL_PRESSED,
                        ALT_PRESSED,
                        SHIFT_PRESSED | ALT_PRESSED,
                        CTRL_PRESSED | ALT_PRESSED,
                        MOD_PRESSED
                    };
                    if (static_cast<unsigned int>(zeroControlKeyState) < ARRAYSIZE(consoleModifierTranslator))
                    {
                        const DWORD winmod = consoleModifierTranslator[zeroControlKeyState];

                        if (zeroVKey == keyEvent->_virtualKeyCode &&
                            AreAllFlagsSet(keyEvent->_activeModifierKeys, winmod) &&
                            AreAllFlagsClear(keyEvent->_activeModifierKeys, ~winmod))
                        {
                            // This really is the character 0x0000
                            *pwchOut = keyEvent->_charData;
                        }
                    }
                }
            }
        }
    }
}

// Routine Description:
// - This routine returns the total number of screen spaces the characters up to the specified character take up.
ULONG RetrieveTotalNumberOfSpaces(_In_ const SHORT sOriginalCursorPositionX,
                                  _In_reads_(ulCurrentPosition) const WCHAR * const pwchBuffer,
                                  _In_ ULONG ulCurrentPosition)
{
    SHORT XPosition = sOriginalCursorPositionX;
    ULONG NumSpaces = 0;

    for (ULONG i = 0; i < ulCurrentPosition; i++)
    {
        WCHAR const Char = pwchBuffer[i];

        ULONG NumSpacesForChar;
        if (Char == UNICODE_TAB)
        {
            NumSpacesForChar = NUMBER_OF_SPACES_IN_TAB(XPosition);
        }
        else if (IS_CONTROL_CHAR(Char))
        {
            NumSpacesForChar = 2;
        }
        else if (IsCharFullWidth(Char))
        {
            NumSpacesForChar = 2;
        }
        else
        {
            NumSpacesForChar = 1;
        }
        XPosition = (SHORT)(XPosition + NumSpacesForChar);
        NumSpaces += NumSpacesForChar;
    }

    return NumSpaces;
}

// Routine Description:
// - This routine returns the number of screen spaces the specified character takes up.
ULONG RetrieveNumberOfSpaces(_In_ SHORT sOriginalCursorPositionX,
                             _In_reads_(ulCurrentPosition + 1) const WCHAR * const pwchBuffer,
                             _In_ ULONG ulCurrentPosition)
{
    WCHAR Char = pwchBuffer[ulCurrentPosition];
    if (Char == UNICODE_TAB)
    {
        ULONG NumSpaces = 0;
        SHORT XPosition = sOriginalCursorPositionX;

        for (ULONG i = 0; i <= ulCurrentPosition; i++)
        {
            Char = pwchBuffer[i];
            if (Char == UNICODE_TAB)
            {
                NumSpaces = NUMBER_OF_SPACES_IN_TAB(XPosition);
            }
            else if (IS_CONTROL_CHAR(Char))
            {
                NumSpaces = 2;
            }
            else if (IsCharFullWidth(Char))
            {
                NumSpaces = 2;
            }
            else
            {
                NumSpaces = 1;
            }
            XPosition = (SHORT)(XPosition + NumSpaces);
        }

        return NumSpaces;
    }
    else if (IS_CONTROL_CHAR(Char))
    {
        return 2;
    }
    else if (IsCharFullWidth(Char))
    {
        return 2;
    }
    else
    {
        return 1;
    }
}

// Routine Description:
// - This routine reads in characters for stream input and does the required processing based on the input mode (line, char, echo).
// - This routine returns UNICODE characters.
// Arguments:
// - pInputBuffer - Pointer to input buffer.
// - Console - Pointer to console buffer information.
// - ScreenInfo - Pointer to screen buffer information.
// - lpBuffer - Pointer to buffer to read into.
// - NumBytes - On input, size of buffer.  On output, number of bytes read.
// - HandleData - Pointer to handle data structure.
// Return Value:
NTSTATUS DoReadConsole(_In_ InputBuffer* pInputBuffer,
                       _In_ HANDLE ProcessData,
                       _Out_writes_bytes_(*pdwNumBytes) WCHAR* pwchBuffer,
                       _Inout_ ULONG* pdwNumBytes,
                       _Inout_ ULONG* pControlKeyState,
                       _In_reads_bytes_opt_(cbInitialData) PWCHAR pwsInitialData,
                       _In_ ULONG cbInitialData,
                       _In_ DWORD dwCtrlWakeupMask,
                       _In_ INPUT_READ_HANDLE_DATA* pHandleData,
                       _In_reads_bytes_opt_(cbExeName) PWCHAR pwsExeName,
                       _In_ ULONG cbExeName,
                       _In_ BOOL fUnicode,
                       _Outptr_result_maybenull_ IWaitRoutine** const ppWaiter)
{
    CONSOLE_INFORMATION* const gci = ServiceLocator::LocateGlobals()->getConsoleInformation();
    LockConsole();
    auto Unlock = wil::ScopeExit([&] { UnlockConsole(); });

    SCREEN_INFORMATION* const pScreenInfo = gci->CurrentScreenBuffer;
    if (nullptr == pScreenInfo)
    {
        return STATUS_UNSUCCESSFUL;
    }

    PCOMMAND_HISTORY const pCommandHistory = FindCommandHistory(ProcessData);

    *ppWaiter = nullptr;

    NTSTATUS Status;
    ULONG NumToWrite;
    BOOL fAddDbcsLead = FALSE;
    ULONG NumToBytes = 0;

    if (*pdwNumBytes < sizeof(WCHAR))
    {
        Status = STATUS_BUFFER_TOO_SMALL;
        return Status;
    }

    DWORD BufferSize = *pdwNumBytes;
    *pdwNumBytes = 0;

    if (IsFlagSet(pHandleData->InputHandleFlags, INPUT_READ_HANDLE_DATA::HandleFlags::InputPending))
    {
        // if we have leftover input, copy as much fits into the user's
        // buffer and return.  we may have multi line input, if a macro
        // has been defined that contains the $T character.

        if (IsFlagSet(pHandleData->InputHandleFlags, INPUT_READ_HANDLE_DATA::HandleFlags::MultiLineInput))
        {
            PWSTR Tmp;

            if (!fUnicode)
            {
                if (pInputBuffer->ReadConInpDbcsLeadByte.Event.KeyEvent.uChar.AsciiChar)
                {
                    fAddDbcsLead = TRUE;
                    *pwchBuffer++ = pInputBuffer->ReadConInpDbcsLeadByte.Event.KeyEvent.uChar.AsciiChar;
                    BufferSize -= sizeof(WCHAR);
                    pHandleData->BytesAvailable -= sizeof(WCHAR);
                    ZeroMemory(&pInputBuffer->ReadConInpDbcsLeadByte, sizeof(INPUT_RECORD));
                }

                if (pHandleData->BytesAvailable == 0 || BufferSize == 0)
                {
                    ClearAllFlags(pHandleData->InputHandleFlags,
                                  (INPUT_READ_HANDLE_DATA::HandleFlags::InputPending |
                                   INPUT_READ_HANDLE_DATA::HandleFlags::MultiLineInput));
                    delete[] pHandleData->BufPtr;
                    *pdwNumBytes = 1;
                    return STATUS_SUCCESS;
                }
                else
                {
                    for (NumToWrite = 0, Tmp = pHandleData->CurrentBufPtr, NumToBytes = 0;
                         NumToBytes < pHandleData->BytesAvailable &&
                         NumToBytes < BufferSize / sizeof(WCHAR) &&
                         *Tmp != UNICODE_LINEFEED;
                         (IsCharFullWidth(*Tmp) ? NumToBytes += 2 : NumToBytes++), Tmp++, NumToWrite += sizeof(WCHAR));
                }
            }

            for (NumToWrite = 0, Tmp = pHandleData->CurrentBufPtr;
                 NumToWrite < pHandleData->BytesAvailable && *Tmp != UNICODE_LINEFEED; Tmp++, NumToWrite += sizeof(WCHAR));
            NumToWrite += sizeof(WCHAR);
            if (NumToWrite > BufferSize)
            {
                NumToWrite = BufferSize;
            }
        }
        else
        {
            if (!fUnicode)
            {
                PWSTR Tmp;

                if (pInputBuffer->ReadConInpDbcsLeadByte.Event.KeyEvent.uChar.AsciiChar)
                {
                    fAddDbcsLead = TRUE;
                    *pwchBuffer++ = pInputBuffer->ReadConInpDbcsLeadByte.Event.KeyEvent.uChar.AsciiChar;
                    BufferSize -= sizeof(WCHAR);
                    pHandleData->BytesAvailable -= sizeof(WCHAR);
                    ZeroMemory(&pInputBuffer->ReadConInpDbcsLeadByte, sizeof(INPUT_RECORD));
                }
                if (pHandleData->BytesAvailable == 0)
                {
                    ClearAllFlags(pHandleData->InputHandleFlags,
                                  (INPUT_READ_HANDLE_DATA::HandleFlags::InputPending |
                                   INPUT_READ_HANDLE_DATA::HandleFlags::MultiLineInput));
                    delete[] pHandleData->BufPtr;
                    *pdwNumBytes = 1;
                    return STATUS_SUCCESS;
                }
                else
                {
                    for (NumToWrite = 0, Tmp = pHandleData->CurrentBufPtr, NumToBytes = 0;
                         NumToBytes < pHandleData->BytesAvailable && NumToBytes < BufferSize / sizeof(WCHAR);
                         (IsCharFullWidth(*Tmp) ? NumToBytes += 2 : NumToBytes++), Tmp++, NumToWrite += sizeof(WCHAR));
                }
            }

            NumToWrite = (BufferSize < pHandleData->BytesAvailable) ? BufferSize : pHandleData->BytesAvailable;
        }

        memmove(pwchBuffer, pHandleData->CurrentBufPtr, NumToWrite);
        pHandleData->BytesAvailable -= NumToWrite;
        if (pHandleData->BytesAvailable == 0)
        {
            ClearAllFlags(pHandleData->InputHandleFlags, (INPUT_READ_HANDLE_DATA::HandleFlags::InputPending | INPUT_READ_HANDLE_DATA::HandleFlags::MultiLineInput));
            delete[] pHandleData->BufPtr;
        }
        else
        {
            pHandleData->CurrentBufPtr = (PWCHAR)((PBYTE)pHandleData->CurrentBufPtr + NumToWrite);
        }

        if (!fUnicode)
        {
            // if ansi, translate string.  we allocated the capture buffer large enough to handle the translated string.
            PCHAR TransBuffer;

            TransBuffer = (PCHAR) new BYTE[NumToBytes];

            if (TransBuffer == nullptr)
            {
                return STATUS_NO_MEMORY;
            }

            NumToWrite = TranslateUnicodeToOem(pwchBuffer, NumToWrite / sizeof(WCHAR), TransBuffer, NumToBytes, &pInputBuffer->ReadConInpDbcsLeadByte);

#pragma prefast(suppress:__WARNING_POTENTIAL_BUFFER_OVERFLOW_HIGH_PRIORITY, "This access is fine but prefast can't follow it, evidently")
            memmove(pwchBuffer, TransBuffer, NumToWrite);

            if (fAddDbcsLead)
            {
                NumToWrite++;
            }

            delete[] TransBuffer;
        }

        *pdwNumBytes = NumToWrite;
        return STATUS_SUCCESS;
    }

    if (IsFlagSet(pInputBuffer->InputMode, ENABLE_LINE_INPUT))
    {
        // read in characters until the buffer is full or return is read.
        // since we may wait inside this loop, store all important variables
        // in the read data structure.  if we do wait, a read data structure
        // will be allocated from the heap and its pointer will be stored
        // in the wait block.  the CookedReadData will be copied into the
        // structure.  the data is freed when the read is completed.

        ULONG i;
        BOOLEAN Echo;
        ConsoleHandleData* pTempHandleData;
        // We need to create a temporary handle to the current screen buffer.
        Status = NTSTATUS_FROM_HRESULT(pScreenInfo->Header.AllocateIoHandle(ConsoleHandleData::HandleType::Output,
                                                                            GENERIC_WRITE,
                                                                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                                                                            &pTempHandleData));

        if (!NT_SUCCESS(Status))
        {
            return Status;
        }

        Echo = !!IsFlagSet(pInputBuffer->InputMode, ENABLE_ECHO_INPUT);


        // to emulate OS/2 KbdStringIn, we read into our own big buffer
        // (256 bytes) until the user types enter.  then return as many
        // chars as will fit in the user's buffer.

        ULONG const TempBufferSize = (BufferSize < LINE_INPUT_BUFFER_SIZE) ? LINE_INPUT_BUFFER_SIZE : BufferSize;
        wchar_t* const TempBuffer = (PWCHAR) new BYTE[TempBufferSize];
        if (TempBuffer == nullptr)
        {
            if (Echo)
            {
                pTempHandleData->CloseHandle();
            }

            return STATUS_NO_MEMORY;
        }

        // Initialize the user's buffer to spaces. This is done so that
        // moving in the buffer via cursor doesn't do strange things.
        for (i = 0; i < TempBufferSize / sizeof(WCHAR); i++)
        {
            TempBuffer[i] = (WCHAR)' ';
        }

        /*
         * Since the console is locked, ScreenInfo is safe. We need to up the
         * ref count to prevent the ScreenInfo from going away while we're
         * waiting for the read to complete.
         */

        COORD invalidCoord;
        invalidCoord.X = -1;
        invalidCoord.Y = -1;
        COOKED_READ_DATA CookedReadData(pInputBuffer,
                                        pHandleData,
                                        pScreenInfo,
                                        TempBufferSize,
                                        0,
                                        0,
                                        TempBuffer,
                                        TempBuffer,
                                        BufferSize,
                                        pwchBuffer,
                                        invalidCoord,
                                        0,
                                        dwCtrlWakeupMask,
                                        pCommandHistory,
                                        Echo,
                                        !!gci->GetInsertMode(),
                                        IsFlagSet(pInputBuffer->InputMode, ENABLE_PROCESSED_INPUT),
                                        IsFlagSet(pInputBuffer->InputMode, ENABLE_LINE_INPUT),
                                        pTempHandleData);

        if (cbInitialData > 0)
        {
            memcpy_s(CookedReadData._BufPtr, CookedReadData._BufferSize, pwsInitialData, cbInitialData);

            CookedReadData._BytesRead += cbInitialData;

            ULONG const cchInitialData = cbInitialData / sizeof(WCHAR);
            CookedReadData._NumberOfVisibleChars = cchInitialData;
            CookedReadData._BufPtr += cchInitialData;
            CookedReadData._CurrentPosition = cchInitialData;

            CookedReadData._OriginalCursorPosition = pScreenInfo->TextInfo->GetCursor()->GetPosition();
            CookedReadData._OriginalCursorPosition.X -= (SHORT)CookedReadData._CurrentPosition;

            const SHORT sScreenBufferSizeX = pScreenInfo->GetScreenBufferSize().X;
            while (CookedReadData._OriginalCursorPosition.X < 0)
            {
                CookedReadData._OriginalCursorPosition.X += sScreenBufferSizeX;
                CookedReadData._OriginalCursorPosition.Y -= 1;
            }
        }

        if (cbExeName > 0)
        {
            CookedReadData.ExeNameLength = (USHORT)cbExeName;
            CookedReadData.ExeName = (PWCHAR) new BYTE[CookedReadData.ExeNameLength];
            memcpy_s(CookedReadData.ExeName, CookedReadData.ExeNameLength, pwsExeName, cbExeName);
        }

        gci->lpCookedReadData = &CookedReadData;

        Status = CookedRead(&CookedReadData, !!fUnicode, pdwNumBytes, pControlKeyState);
        if (CONSOLE_STATUS_WAIT == Status)
        {
            COOKED_READ_DATA* pCookedReadWaiter = new COOKED_READ_DATA(std::move(CookedReadData));
            if (nullptr == pCookedReadWaiter)
            {
                Status = STATUS_NO_MEMORY;
            }
            else
            {
                gci->lpCookedReadData = pCookedReadWaiter;
                *ppWaiter = pCookedReadWaiter;
            }
        }

        if (CONSOLE_STATUS_WAIT != Status)
        {
            gci->lpCookedReadData = nullptr;
        }

        return Status;
    }
    else
    {
        // Character (raw) mode.

        // Read at least one character in. After one character has been read,
        // get any more available characters and return. The first call to
        // GetChar may block. If we do wait, a read data structure will be
        // allocated from the heap and its pointer will be stored in the wait
        // block. The RawReadData will be copied into the structure. The data
        // is freed when the read is completed.

        RAW_READ_DATA RawReadData(pInputBuffer,
                                  pHandleData,
                                  BufferSize,
                                  pwchBuffer);

        if (*pdwNumBytes < BufferSize)
        {
            PWCHAR pwchBufferTmp = pwchBuffer;

            NumToWrite = 0;
            if (!fUnicode)
            {
                if (pInputBuffer->ReadConInpDbcsLeadByte.Event.KeyEvent.uChar.AsciiChar)
                {
                    fAddDbcsLead = TRUE;
                    *pwchBuffer++ = pInputBuffer->ReadConInpDbcsLeadByte.Event.KeyEvent.uChar.AsciiChar;
                    BufferSize -= sizeof(WCHAR);
                    ZeroMemory(&pInputBuffer->ReadConInpDbcsLeadByte, sizeof(INPUT_RECORD));
                    Status = STATUS_SUCCESS;
                    if (BufferSize == 0)
                    {
                        *pdwNumBytes = 1;
                        return STATUS_SUCCESS;
                    }
                }
                else
                {
                    Status = GetChar(pInputBuffer,
                                     pwchBuffer,
                                     true,
                                     nullptr,
                                     nullptr,
                                     nullptr);
                }
            }
            else
            {
                Status = GetChar(pInputBuffer,
                                 pwchBuffer,
                                 true,
                                 nullptr,
                                 nullptr,
                                 nullptr);
            }

            if (Status == CONSOLE_STATUS_WAIT)
            {
                *ppWaiter = new RAW_READ_DATA(std::move(RawReadData));
            }

            if (!NT_SUCCESS(Status))
            {
                *pdwNumBytes = 0;
                return Status;
            }

            if (!fAddDbcsLead)
            {
                IsCharFullWidth(*pwchBuffer) ? *pdwNumBytes += 2 : ++*pdwNumBytes;
                NumToWrite += sizeof(WCHAR);
                pwchBuffer++;
            }

            while (NumToWrite < BufferSize)
            {
                Status = GetChar(pInputBuffer,
                                 pwchBuffer,
                                 false,
                                 nullptr,
                                 nullptr,
                                 nullptr);
                if (!NT_SUCCESS(Status))
                {
                    break;
                }
                IsCharFullWidth(*pwchBuffer) ? *pdwNumBytes += 2 : ++*pdwNumBytes;
                pwchBuffer++;
                NumToWrite += sizeof(WCHAR);
            }

            // if ansi, translate string.  we allocated the capture buffer large enough to handle the translated string.
            if (!fUnicode)
            {
                PCHAR TransBuffer;

                TransBuffer = (PCHAR) new BYTE[*pdwNumBytes];
                if (TransBuffer == nullptr)
                {
                    return STATUS_NO_MEMORY;
                }

                pwchBuffer = pwchBufferTmp;

                *pdwNumBytes = TranslateUnicodeToOem(pwchBuffer,
                                                     NumToWrite / sizeof(WCHAR),
                                                     TransBuffer,
                                                     *pdwNumBytes,
                                                     &pInputBuffer->ReadConInpDbcsLeadByte);

#pragma prefast(suppress:26053 26015, "PREfast claims read overflow. *pdwNumBytes is the exact size of TransBuffer as allocated above.")
                memmove(pwchBuffer, TransBuffer, *pdwNumBytes);

                if (fAddDbcsLead)
                {
                    ++*pdwNumBytes;
                }

                delete[] TransBuffer;
            }
            else
            {
                // We always return the byte count for A & W modes, so in the Unicode case where we didn't translate back,
                // set the return to the byte count that we assembled while pulling characters from the internal buffers.
                *pdwNumBytes = NumToWrite;
            }
        }
    }

    return STATUS_SUCCESS;
}

HRESULT ApiRoutines::ReadConsoleAImpl(_In_ IConsoleInputObject* const pInContext,
                                      _Out_writes_to_(cchTextBuffer, *pcchTextBufferWritten) char* const psTextBuffer,
                                      _In_ size_t const cchTextBuffer,
                                      _Out_ size_t* const pcchTextBufferWritten,
                                      _Outptr_result_maybenull_ IWaitRoutine** const ppWaiter,
                                      _In_reads_opt_(cchInitialData) char* const psInitialData,
                                      _In_ size_t const cchInitialData,
                                      _In_reads_opt_(cchExeName) wchar_t* const pwsExeName,
                                      _In_ size_t const cchExeName,
                                      _In_ INPUT_READ_HANDLE_DATA* const pHandleData,
                                      _In_ HANDLE const hConsoleClient,
                                      _In_ DWORD const dwControlWakeupMask,
                                      _Out_ DWORD* const pdwControlKeyState)
{
    ULONG ulTextBuffer;
    RETURN_IF_FAILED(SizeTToULong(cchTextBuffer, &ulTextBuffer));

    size_t cbExeName;
    RETURN_IF_FAILED(SizeTMult(cchExeName, sizeof(wchar_t), &cbExeName));

    ULONG ulExeName;
    RETURN_IF_FAILED(SizeTToULong(cbExeName, &ulExeName));

    ULONG ulInitialData;
    RETURN_IF_FAILED(SizeTToULong(cchInitialData, &ulInitialData));

    // DoReadConsole performs the same check, but having this here
    // before the function call makes the static analyzer happy.
    if (ulTextBuffer < sizeof(WCHAR))
    {
        return HRESULT_FROM_NT(STATUS_BUFFER_TOO_SMALL);
    }

    NTSTATUS const Status = DoReadConsole(pInContext,
                                          hConsoleClient,
                                          (wchar_t*)psTextBuffer,
                                          &ulTextBuffer,
                                          pdwControlKeyState,
                                          (wchar_t*)psInitialData,
                                          ulInitialData,
                                          dwControlWakeupMask,
                                          pHandleData,
                                          pwsExeName,
                                          ulExeName,
                                          false,
                                          ppWaiter);

    *pcchTextBufferWritten = ulTextBuffer;

    return HRESULT_FROM_NT(Status);
}

HRESULT ApiRoutines::ReadConsoleWImpl(_In_ IConsoleInputObject* const pInContext,
                                      _Out_writes_to_(cchTextBuffer, *pcchTextBufferWritten) wchar_t* const pwsTextBuffer,
                                      _In_ size_t const cchTextBuffer,
                                      _Out_ size_t* const pcchTextBufferWritten,
                                      _Outptr_result_maybenull_ IWaitRoutine** const ppWaiter,
                                      _In_reads_opt_(cchInitialData) wchar_t* const pwsInitialData,
                                      _In_ size_t const cchInitialData,
                                      _In_reads_opt_(cchExeName) wchar_t* const pwsExeName,
                                      _In_ size_t const cchExeName,
                                      _In_ INPUT_READ_HANDLE_DATA* const pHandleData,
                                      _In_ HANDLE const hConsoleClient,
                                      _In_ DWORD const dwControlWakeupMask,
                                      _Out_ DWORD* const pdwControlKeyState)
{
    size_t cbTextBuffer;
    RETURN_IF_FAILED(SizeTMult(cchTextBuffer, sizeof(wchar_t), &cbTextBuffer));

    ULONG ulTextBuffer;
    RETURN_IF_FAILED(SizeTToULong(cbTextBuffer, &ulTextBuffer));

    size_t cbInitialData;
    RETURN_IF_FAILED(SizeTMult(cchInitialData, sizeof(wchar_t), &cbInitialData));

    ULONG ulInitialData;
    RETURN_IF_FAILED(SizeTToULong(cbInitialData, &ulInitialData));

    size_t cbExeName;
    RETURN_IF_FAILED(SizeTMult(cchExeName, sizeof(wchar_t), &cbExeName));

    ULONG ulExeName;
    RETURN_IF_FAILED(SizeTToULong(cbExeName, &ulExeName));

    NTSTATUS const Status = DoReadConsole(pInContext,
                                          hConsoleClient,
                                          pwsTextBuffer,
                                          &ulTextBuffer,
                                          pdwControlKeyState,
                                          pwsInitialData,
                                          ulInitialData,
                                          dwControlWakeupMask,
                                          pHandleData,
                                          pwsExeName,
                                          ulExeName,
                                          true,
                                          ppWaiter);

    assert(ulTextBuffer % sizeof(wchar_t) == 0);
    *pcchTextBufferWritten = ulTextBuffer / sizeof(wchar_t);

    return HRESULT_FROM_NT(Status);
}

VOID UnblockWriteConsole(_In_ const DWORD dwReason)
{
    CONSOLE_INFORMATION* const gci = ServiceLocator::LocateGlobals()->getConsoleInformation();
    gci->Flags &= ~dwReason;

    if (AreAllFlagsClear(gci->Flags, (CONSOLE_SUSPENDED | CONSOLE_SELECTING | CONSOLE_SCROLLBAR_TRACKING)))
    {
        // There is no longer any reason to suspend output, so unblock it.
        gci->OutputQueue.NotifyWaiters(true);
    }
}

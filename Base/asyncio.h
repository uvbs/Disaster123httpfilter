//------------------------------------------------------------------------------
// File: AsyncIo.h
//
// Desc: DirectShow sample code - base library for I/O functionality.
//
// Copyright (c) Microsoft Corporation.  All rights reserved.
//------------------------------------------------------------------------------


#ifndef __ASYNCIO_H__
#define __ASYNCIO_H__

//
// definition of CAsyncFile object that performs file access. It provides
// asynchronous, unbuffered, aligned reads from a file, using a worker thread
// and potentially overlapped i/o if available.

// Remarks:

// CAsyncStream is an abstract base class that models I/O operations. 
// The CAsyncReader constructor takes a pointer to this class. The 
// derived filter class is expected to implement I/O operations using 
// the interface defined by this CAsyncStream.

// In the original Async sample, CAsyncStream has a method named
// Read() that performs a read operation. In this version, that
// method is replaced by StartRead() and EndRead().

class CAsyncIo;
class CAsyncStream;

//
//  Model the stream we read from based on a file-like interface
//
class CAsyncStream
{
public:
    virtual ~CAsyncStream() {};

    virtual HRESULT StartRead(
		PBYTE pbBuffer,
		DWORD dwBytesToRead,
		BOOL bAlign,
		/* in */  LPOVERLAPPED pOverlapped,
		/* out */ LPBOOL pbPending,     // Receives TRUE if I/O request is pending.
		/* out */ LPDWORD pdwBytesRead  // Receives the number of bytes read, if
                                        // *pbPending is FALSE. Otherwise, ignore.
		) = 0;

	virtual HRESULT EndRead(LPOVERLAPPED pOverlapped, LPDWORD pdwBytesRead) = 0;

	virtual HRESULT Cancel() = 0;
    virtual HRESULT Length(LONGLONG *pTotal, LONGLONG *pAvailable) = 0;
    // NOTE: In the IAsyncReader::Length method, pAvailable can be NULL.
    // However, CAsyncStream::Length is never called with pAvailable equal
    // to NULL (because CAsyncIo will always pass in a valid pointer).

    virtual DWORD Alignment() = 0;

    virtual void Lock() = 0;        // For serializing calls to this object.
    virtual void Unlock() = 0;
};

// represents a single request and performs the i/o. Can be called on either
// worker thread or app thread.
class CAsyncRequest
{
    CAsyncIo     *m_pIo;
    CAsyncStream *m_pStream;
    LONGLONG      m_llPos;
    LONG        m_lLength;
	BOOL		m_bPending;
	DWORD		m_dwActual;
    BYTE*       m_pBuffer;
    LPVOID      m_pContext;
    DWORD_PTR   m_dwUser;
    HRESULT     m_hr;
	OVERLAPPED	m_Overlapped;

public:

	CAsyncRequest() 
	{
		ZeroMemory(&m_Overlapped, sizeof(m_Overlapped));
	}

	virtual ~CAsyncRequest()
	{
		CloseHandle(m_Overlapped.hEvent);
	}

    // init the params for this request. Issue the i/o
    // if overlapped i/o is possible.
    HRESULT Request(
        CAsyncIo *pIo,
        CAsyncStream *pStream,
        LONGLONG llPos,
        LONG lLength,
        BOOL bAligned,
        BYTE* pBuffer,
        LPVOID pContext,    // filter's context
        DWORD_PTR dwUser);      // downstream filter's context

    // issue the i/o if not overlapped, and block until i/o complete.
    // returns error code of file i/o
    HRESULT Complete();

    // cancels the i/o. 
    HRESULT Cancel()
    {
		return m_pStream->Cancel();
    };

    // accessor functions
    LPVOID GetContext()
    {
        return m_pContext;
    };

    DWORD_PTR GetUser()
    {
        return m_dwUser;
    };

    HRESULT GetHResult() {
        return m_hr;
    };

    // we set m_lLength to the actual length
    LONG GetActualLength() {
        return m_lLength;
    };

    LONGLONG GetStart() {
        return m_llPos;
    };
};


typedef CGenericList<CAsyncRequest> CRequestList;

// this class needs a worker thread, but the ones defined in classes\base
// are not suitable (they assume you have one message sent or posted per
// request, whereas here for efficiency we want just to set an event when
// there is work on the queue).
//
// we create CAsyncRequest objects and queue them on m_listWork. The worker
// thread pulls them off, completes them and puts them on m_listDone.
// The events m_evWork and m_evDone are set when the corresponding lists are
// not empty.
//
// Synchronous requests are done on the caller thread. These should be
// synchronised by the caller, but to make sure we hold m_csFile across
// the SetFilePointer/ReadFile code.
//
// Flush by calling BeginFlush. This rejects all further requests (by
// setting m_bFlushing within m_csLists), cancels all requests and moves them
// to the done list, and sets m_evDone to ensure that no WaitForNext operations
// will block. Call EndFlush to cancel this state.
//
// we support unaligned calls to SyncRead. This is done by opening the file
// twice if we are using unbuffered i/o (m_dwAlign > 1).
// !!!fix this to buffer on top of existing file handle?
class CAsyncIo
{

    CCritSec m_csReader;
    CAsyncStream *m_pStream;

    CCritSec m_csLists;      // locks access to the list and events
    BOOL m_bFlushing;        // true if between BeginFlush/EndFlush

    CRequestList m_listWork;
    CRequestList m_listDone;

    CAMEvent m_evWork;      // set when list is not empty
    CAMEvent m_evDone;

    // for correct flush behaviour: all protected by m_csLists
    LONG    m_cItemsOut;    // nr of items not on listDone or listWork
    BOOL    m_bWaiting;     // TRUE if someone waiting for m_evAllDone
    CAMEvent m_evAllDone;   // signal when m_cItemsOut goes to 0 if m_cWaiting


    CAMEvent m_evStop;         // set when thread should exit
    HANDLE m_hThread;

    // start the thread
    HRESULT StartThread(void);

    // stop the thread and close the handle
    HRESULT CloseThread(void);

    // manage the list of requests. hold m_csLists and ensure
    // that the (manual reset) event hevList is set when things on
    // the list but reset when the list is empty.
    // returns null if list empty
    CAsyncRequest* GetWorkItem();

    // get an item from the done list
    CAsyncRequest* GetDoneItem();

    // put an item on the work list
    HRESULT PutWorkItem(CAsyncRequest* pRequest);

    // put an item on the done list
    HRESULT PutDoneItem(CAsyncRequest* pRequest);

    // called on thread to process any active requests
    void ProcessRequests(void);

    // initial static thread proc calls ThreadProc with DWORD
    // param as this
    static DWORD WINAPI InitialThreadProc(LPVOID pv) {
        CAsyncIo * pThis = (CAsyncIo*) pv;
        return pThis->ThreadProc();
    };

    DWORD ThreadProc(void);

public:

    CAsyncIo(CAsyncStream *pStream);
    ~CAsyncIo();

    // open the file
    HRESULT Open(LPCTSTR pName);

    // ready for async activity - call this before
    // calling Request
    HRESULT AsyncActive(void);

    // call this when no more async activity will happen before
    // the next AsyncActive call
    HRESULT AsyncInactive(void);

    // queue a requested read. must be aligned.
    HRESULT Request(
            LONGLONG llPos,
            LONG lLength,
            BOOL bAligned,
            BYTE* pBuffer,
            LPVOID pContext,
            DWORD_PTR dwUser);

    // wait for the next read to complete
    HRESULT WaitForNext(
            DWORD dwTimeout,
            LPVOID *ppContext,
            DWORD_PTR * pdwUser,
            LONG * pcbActual);

    // perform a read of an already aligned buffer
    HRESULT SyncReadAligned(
            LONGLONG llPos,
            LONG lLength,
            BYTE* pBuffer,
            LONG* pcbActual,
            PVOID pvContext);

    // perform a synchronous read. will be buffered
    // if not aligned.
    HRESULT SyncRead(
            LONGLONG llPos,
            LONG lLength,
            BYTE* pBuffer);

    // return length
    HRESULT Length(LONGLONG *pllTotal, LONGLONG* pllAvailable);

    // all Reader positions, read lengths and memory locations must
    // be aligned to this.
    HRESULT Alignment(LONG* pl);

    HRESULT BeginFlush();
    HRESULT EndFlush();

    LONG Alignment()
    {
        return m_pStream->Alignment();
    };

    BOOL IsAligned(LONG l) {
    if ((l & (Alignment() -1)) == 0) {
        return TRUE;
    } else {
        return FALSE;
    }
    };

    BOOL IsAligned(LONGLONG ll) {
        return IsAligned( (LONG) (ll & 0xffffffff));
    };

    //  Accessor
    HANDLE StopEvent() const { return m_evDone; }
};

#endif // __ASYNCIO_H__

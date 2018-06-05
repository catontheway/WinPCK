//////////////////////////////////////////////////////////////////////
// PckClassThread.cpp: 用于解析完美世界公司的pck文件中的数据，并显示在List中
// 主要功能，包括压缩、更新、重命名等
//
// 此程序由 李秋枫/stsm/liqf 编写
//
// 此代码预计将会开源，任何基于此代码的修改发布请保留原作者信息
// 
// 2012.4.10
//////////////////////////////////////////////////////////////////////

#include "PckClass.h"
#include <process.h>
#include "PckClassFileDisk.h"

#pragma warning ( disable : 4996 )
#pragma warning ( disable : 4244 )
#pragma warning ( disable : 4267 )
#pragma warning ( disable : 4311 )
#pragma warning ( disable : 4302 )

#include "PckClassThreadVariables.cpp"
#include "PckClassThreadCreate.cpp"
#include "PckClassThreadAddFile.cpp"
#include "PckClassThreadRecompress.cpp"
/*
** 多线程压缩时相关参数的初始化
**
**
*/

BOOL CPckClass::BeforeSingleOrMultiThreadProcess(LPPCK_ALL_INFOS lpPckAllInfo, CMapViewFileWrite* &lpWrite, LPCTSTR szPckFile, DWORD dwCreationDisposition, QWORD qdwSizeToMap, int threadnum)
{
	//构造头和尾时需要的参数
	memset(lpPckAllInfo, 0, sizeof(PCK_ALL_INFOS));
	lpPckAllInfo->lpSaveAsPckVerFunc = m_PckAllInfo.lpSaveAsPckVerFunc;

	//开始压缩
	//以下是创建一个文件，用来保存压缩后的文件
	lpWrite = new CMapViewFileWrite(m_PckAllInfo.lpSaveAsPckVerFunc->cPckXorKeys->dwMaxSinglePckSize);

	//OPEN_ALWAYS，新建新的pck(CREATE_ALWAYS)或更新存在的pck(OPEN_EXISTING)
	if(!lpWrite->OpenPckAndMappingWrite(szPckFile, dwCreationDisposition, qdwSizeToMap)) {

		delete lpWrite;
		return FALSE;
	}

	m_lpPckParams->cVarParams.dwUIProgress = 0;

	//多线程使用参数初始化

	mt_lpbThreadRunning = &m_lpPckParams->cVarParams.bThreadRunning;	//监视线程暂停
	mt_lpdwCount = &m_lpPckParams->cVarParams.dwUIProgress;			//dwCount;
	mt_MaxMemory = m_lpPckParams->dwMTMaxMemory;						//可用最大缓存;
	mt_lpMaxMemory = &m_lpPckParams->cVarParams.dwMTMemoryUsed;		//已使用缓存;
	mt_nMallocBlocked = 0;											//等待的线程数
	
	return TRUE;
}

void CPckClass::MultiThreadInitialize(VOID CompressThread(VOID*), VOID WriteThread(VOID*), int threadnum)
{
	mt_evtAllWriteFinish = CreateEventA(NULL, FALSE, FALSE, m_szEventAllWriteFinish);
	mt_evtAllCompressFinish = CreateEventA(NULL, TRUE, FALSE, m_szEventAllCompressFinish);
	mt_evtMaxMemory = CreateEventA(NULL, FALSE, FALSE, m_szEventMaxMemory);

	mt_threadID = 0;

	InitializeCriticalSection(&g_cs);
	InitializeCriticalSection(&g_mt_threadID);
	InitializeCriticalSection(&g_mt_nMallocBlocked);
	InitializeCriticalSection(&g_mt_lpMaxMemory);
	InitializeCriticalSection(&g_dwCompressedflag);
	InitializeCriticalSection(&g_mt_pckCompressedDataPtrArrayPtr);

	for(int i = 0; i < threadnum; i++) {
		_beginthread(CompressThread, 0, this);
	}
	_beginthread(WriteThread, 0, this);

	WaitForSingleObject(mt_evtAllCompressFinish, INFINITE);

	//SetEvent(mt_evtToWrite);		//防止Write线程卡住 
	WaitForSingleObject(mt_evtAllWriteFinish, INFINITE);

	DeleteCriticalSection(&g_cs);
	DeleteCriticalSection(&g_mt_threadID);
	DeleteCriticalSection(&g_mt_nMallocBlocked);
	DeleteCriticalSection(&g_mt_lpMaxMemory);
	DeleteCriticalSection(&g_dwCompressedflag);
	DeleteCriticalSection(&g_mt_pckCompressedDataPtrArrayPtr);

	CloseHandle(mt_evtMaxMemory);
	CloseHandle(mt_evtAllCompressFinish);
	CloseHandle(mt_evtAllWriteFinish);

	if(!m_lpPckParams->cVarParams.bThreadRunning) {
		m_PckLog.PrintLogW(TEXT_USERCANCLE);
	}

}

//重命名文件
BOOL CPckClass::RenameFilename()
{

	m_PckLog.PrintLogI(TEXT_LOG_RENAME);

	int			level = m_lpPckParams->dwCompressLevel;
	DWORD		IndexCompressedFilenameDataLengthCryptKey1 = m_PckAllInfo.lpSaveAsPckVerFunc->cPckXorKeys->IndexCompressedFilenameDataLengthCryptKey1;
	DWORD		IndexCompressedFilenameDataLengthCryptKey2 = m_PckAllInfo.lpSaveAsPckVerFunc->cPckXorKeys->IndexCompressedFilenameDataLengthCryptKey2;

	//以下是创建一个文件，用来保存压缩后的文件
	CMapViewFileWrite cFileWrite(m_PckAllInfo.lpSaveAsPckVerFunc->cPckXorKeys->dwMaxSinglePckSize);

	if(!cFileWrite.OpenPck(m_PckAllInfo.szFilename, OPEN_EXISTING)) {

		m_PckLog.PrintLogEL(TEXT_OPENWRITENAME_FAIL, m_PckAllInfo.szFilename, __FILE__, __FUNCTION__, __LINE__);
		assert(FALSE);
		return FALSE;
	}

	QWORD dwFileSize = NPckClassFileDisk::GetPckFilesizeRename(m_PckAllInfo.szFilename, cFileWrite.GetFileSize());//cFileWrite.GetFileSize() + PCK_RENAME_EXPAND_ADD;

	if(!cFileWrite.Mapping(dwFileSize)) {

		m_PckLog.PrintLogEL(TEXT_CREATEMAPNAME_FAIL, m_PckAllInfo.szFilename, __FILE__, __FUNCTION__, __LINE__);
		assert(FALSE);
		return FALSE;
	}

	//写文件索引
	QWORD dwAddress = m_PckAllInfo.dwAddressOfFilenameIndex;

	//窗口中以显示的文件进度，初始化，显示写索引进度mt_hFileToWrite
	m_lpPckParams->cVarParams.dwUIProgress = 0;
	m_lpPckParams->cVarParams.dwUIProgressUpper = m_PckAllInfo.dwFileCount;

	//写原来的文件
	LPPCKINDEXTABLE	lpPckIndexTableSrc = m_PckAllInfo.lpPckIndexTable;

	//写入索引
	for(DWORD i = 0; i < m_lpPckParams->cVarParams.dwUIProgressUpper; ++i) {

		PCKINDEXTABLE_COMPRESS	pckIndexTableTemp;

		if(lpPckIndexTableSrc->bSelected) {
			--m_PckAllInfo.dwFileCount;
			++lpPckIndexTableSrc;

			//窗口中以显示的文件进度
			++m_lpPckParams->cVarParams.dwUIProgress;
			continue;
		}

		FillAndCompressIndexData(&pckIndexTableTemp, &lpPckIndexTableSrc->cFileIndex);

		WritePckIndex(&cFileWrite, &pckIndexTableTemp, dwAddress);

		++lpPckIndexTableSrc;

	}

	AfterProcess(&cFileWrite, m_PckAllInfo, dwAddress, FALSE);

	m_PckLog.PrintLogI(TEXT_LOG_WORKING_DONE);

	return TRUE;
}


void CPckClass::detectMaxAndAddMemory(DWORD dwMallocSize)
{
	EnterCriticalSection(&g_mt_lpMaxMemory);

	if((*mt_lpMaxMemory) >= mt_MaxMemory) {
		LeaveCriticalSection(&g_mt_lpMaxMemory);
		{
			EnterCriticalSection(&g_mt_nMallocBlocked);
			mt_nMallocBlocked++;
			LeaveCriticalSection(&g_mt_nMallocBlocked);
		}
		WaitForSingleObject(mt_evtMaxMemory, INFINITE);
		{
			EnterCriticalSection(&g_mt_nMallocBlocked);
			mt_nMallocBlocked--;
			LeaveCriticalSection(&g_mt_nMallocBlocked);
		}
	} else
		LeaveCriticalSection(&g_mt_lpMaxMemory);

	{
		EnterCriticalSection(&g_mt_lpMaxMemory);
		(*mt_lpMaxMemory) += dwMallocSize;
		LeaveCriticalSection(&g_mt_lpMaxMemory);
	}
}

void CPckClass::freeMaxAndSubtractMemory(DWORD dwMallocSize)
{
	EnterCriticalSection(&g_mt_lpMaxMemory);
	(*mt_lpMaxMemory) -= dwMallocSize;
	if((*mt_lpMaxMemory) < mt_MaxMemory) {
		LeaveCriticalSection(&g_mt_lpMaxMemory);

		EnterCriticalSection(&g_mt_nMallocBlocked);
		int nMallocBlocked = mt_nMallocBlocked;
		LeaveCriticalSection(&g_mt_nMallocBlocked);
		for(int i = 0; i < nMallocBlocked; i++)
			SetEvent(mt_evtMaxMemory);

	} else {
		LeaveCriticalSection(&g_mt_lpMaxMemory);
	}
}

BOOL CPckClass::initCompressedDataQueue(int threadnum, DWORD dwFileCount, QWORD dwAddressStartAt)
{
	//申请空间,文件名压缩数据 数组
	if(NULL == (mt_lpPckIndexTable = new PCKINDEXTABLE_COMPRESS[dwFileCount])) {
		m_PckLog.PrintLogEL(TEXT_MALLOC_FAIL, __FILE__, __FUNCTION__, __LINE__);
		assert(FALSE);
		return FALSE;
	}
	memset(mt_lpPckIndexTable, 0, sizeof(PCKINDEXTABLE_COMPRESS) * dwFileCount);

	if(NULL == (mt_pckCompressedDataPtrArray = (BYTE**)malloc(sizeof(BYTE*) * (dwFileCount + 1)))) {
		m_PckLog.PrintLogEL(TEXT_MALLOC_FAIL, __FILE__, __FUNCTION__, __LINE__);
		assert(FALSE);
		return FALSE;
	}
	memset(mt_pckCompressedDataPtrArray, 0, sizeof(BYTE*) * (dwFileCount + 1));

	mt_lpPckIndexTablePut = mt_lpPckIndexTableGet = mt_lpPckIndexTable;
	mt_pckCompressedDataPtrArrayPut = mt_pckCompressedDataPtrArrayGet = mt_pckCompressedDataPtrArray;
	mt_dwMaxQueueLength = dwFileCount;
#ifdef _DEBUG
	mt_dwCurrentQueuePosPut = mt_dwCurrentQueuePosGet = 0;
#endif
	mt_dwCurrentQueueLength = 0;
	mt_dwAddressNameQueue = mt_dwAddressQueue = dwAddressStartAt;


	return TRUE;
}

BOOL CPckClass::putCompressedDataQueue(LPBYTE lpBuffer, LPPCKINDEXTABLE_COMPRESS lpPckIndexTableComped, LPPCKFILEINDEX lpPckFileIndexToCompress)
{
	//先不考虑ptr>count的情况

	lpPckIndexTableComped->dwCompressedFilesize = lpPckFileIndexToCompress->dwFileCipherTextSize;
	EnterCriticalSection(&g_mt_pckCompressedDataPtrArrayPtr);
	//lpPckIndexTableComped->dwAddressFileDataToWrite = lpPckFileIndexToCompress->dwAddressOffset = mt_dwAddressQueue;

	if(0 == lpPckIndexTableComped->dwAddressOfDuplicateOldDataArea) {
		lpPckIndexTableComped->dwAddressFileDataToWrite = lpPckFileIndexToCompress->dwAddressOffset = mt_dwAddressQueue;
		mt_dwAddressQueue += lpPckFileIndexToCompress->dwFileCipherTextSize;
		lpPckIndexTableComped->dwAddressAddStep = lpPckFileIndexToCompress->dwFileCipherTextSize;
	} else {
		lpPckIndexTableComped->dwAddressFileDataToWrite = lpPckIndexTableComped->dwAddressOfDuplicateOldDataArea;
		lpPckIndexTableComped->dwAddressAddStep = 0;
	}

	if(!lpPckIndexTableComped->bInvalid) {
		FillAndCompressIndexData(lpPckIndexTableComped, lpPckFileIndexToCompress);
	}

#ifdef _DEBUG
	if(1 == (int)lpBuffer) {
		logOutput(__FUNCTION__, formatString("id(%05d), lpBuffer(0x%08x):0x0000000000000000, lpPckIndexTable(0x%08x), szFile(%s)\r\n", mt_dwCurrentQueuePosPut, (int)lpBuffer, (int)mt_lpPckIndexTablePut, lpPckFileIndexToCompress->szFilename));
	} else {
		logOutput(__FUNCTION__, formatString("id(%05d), lpBuffer(0x%08x):0x%016llx, lpPckIndexTable(0x%08x), szFile(%s)\r\n", mt_dwCurrentQueuePosPut, (int)lpBuffer, _byteswap_uint64(*(unsigned __int64*)lpBuffer), (int)mt_lpPckIndexTablePut, lpPckFileIndexToCompress->szFilename));
	}

	mt_dwCurrentQueuePosPut++;

	assert(NULL != lpBuffer);
#endif

	(*mt_pckCompressedDataPtrArrayPut) = lpBuffer;
	memcpy(mt_lpPckIndexTablePut, lpPckIndexTableComped, sizeof(PCKINDEXTABLE_COMPRESS));

	mt_dwCurrentQueueLength++;
	mt_lpPckIndexTablePut++;
	mt_pckCompressedDataPtrArrayPut++;
	LeaveCriticalSection(&g_mt_pckCompressedDataPtrArrayPtr);

	return TRUE;
}

BOOL CPckClass::getCompressedDataQueue(LPBYTE &lpBuffer, LPPCKINDEXTABLE_COMPRESS &lpPckIndexTable)
{

	EnterCriticalSection(&g_mt_pckCompressedDataPtrArrayPtr);
	if(mt_dwCurrentQueueLength) {

		mt_dwCurrentQueueLength--;
		lpBuffer = *mt_pckCompressedDataPtrArrayGet;
		lpPckIndexTable = mt_lpPckIndexTableGet;

		LeaveCriticalSection(&g_mt_pckCompressedDataPtrArrayPtr);

#ifdef _DEBUG

		assert(NULL != lpBuffer);

		if(0 != lpPckIndexTable->dwIndexDataLength) {
			char szFile[MAX_INDEXTABLE_CLEARTEXT_LENGTH];
			DWORD dwTest = MAX_INDEXTABLE_CLEARTEXT_LENGTH;

			decompress(szFile, &dwTest, lpPckIndexTable->buffer, lpPckIndexTable->dwIndexDataLength);
			if(1 == (int)lpBuffer) {
				logOutput(__FUNCTION__, formatString("id(%05d), lpBuffer(0x%08x):0x0000000000000000, lpPckIndexTable(0x%08x), szFile(%s)\r\n", mt_dwCurrentQueuePosGet, (int)lpBuffer, (int)lpPckIndexTable, szFile));
			} else {
				logOutput(__FUNCTION__, formatString("id(%05d), lpBuffer(0x%08x):0x%016llx, lpPckIndexTable(0x%08x), szFile(%s)\r\n", mt_dwCurrentQueuePosGet, (int)lpBuffer, _byteswap_uint64(*(unsigned __int64*)lpBuffer), (int)lpPckIndexTable, szFile));
			}
		}
		mt_dwCurrentQueuePosGet++;
#endif

		mt_lpPckIndexTableGet++;
		mt_pckCompressedDataPtrArrayGet++;

		return TRUE;
	} else {
		LeaveCriticalSection(&g_mt_pckCompressedDataPtrArrayPtr);
		return FALSE;
	}
}

void CPckClass::uninitCompressedDataQueue(int threadnum)
{
	delete[] mt_lpPckIndexTable;

	free(mt_pckCompressedDataPtrArray);
}
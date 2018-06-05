
#define COMPRESSTHREADFUNC CompressThreadCreate
#define COMPRESSSINGLETHREADFUNC CreatePckFileSingleThread
#define TARGET_PCK_MODE_COMPRESS PCK_MODE_COMPRESS_CREATE
#include "PckClassThreadCompressFunctions.h"
#include "PckClassFileDisk.h"


VOID CPckClass::WriteThread(VOID* pParam)
{
	CPckClass *pThis = (CPckClass*)pParam;

	LPBYTE		dataToWrite;
	QWORD		dwTotalFileSizeAfterCompress = mt_CompressTotalFileSize;
	LPBYTE		lpBufferToWrite;
	LPPCKINDEXTABLE_COMPRESS	lpPckIndexTableComp;

	QWORD						dwAddressDataAreaEndAt = mt_dwAddressNameQueue;
	DWORD						nWrite = 0;

	while(1) {
		if(pThis->getCompressedDataQueue(dataToWrite, lpPckIndexTableComp)) {
			QWORD dwAddress = lpPckIndexTableComp->dwAddressFileDataToWrite;

			//此处代码应该在WriteThread线程中实现
			//判断一下dwAddress的值会不会超过dwTotalFileSizeAfterCompress
			//如果超过，说明文件空间申请的过小，重新申请一下ReCreateFileMapping
			//新文件大小在原来的基础上增加(lpfirstFile->dwFileSize + 1mb) >= 64mb ? (lpfirstFile->dwFileSize + 1mb) :64mb
			if(!NPckClassFileDisk::IsNeedExpandWritingFile(mt_lpFileWrite, dwAddress, lpPckIndexTableComp->dwCompressedFilesize, dwTotalFileSizeAfterCompress)) {

				pThis->AfterWriteThreadFailProcess(
					mt_lpbThreadRunning,
					mt_evtAllCompressFinish,
					mt_dwFileCount,
					nWrite,
					mt_dwAddressQueue,
					dwAddressDataAreaEndAt,
					mt_pckCompressedDataPtrArrayGet);

				break;

			}

			//处理lpPckFileIndex->dwAddressOffset
			if(0 != lpPckIndexTableComp->dwCompressedFilesize) {

				if(NULL == (lpBufferToWrite = mt_lpFileWrite->View(dwAddress, lpPckIndexTableComp->dwCompressedFilesize))) {
					//写过程中目标文件无法View
					mt_lpFileWrite->UnMaping();

					pThis->AfterWriteThreadFailProcess(
						mt_lpbThreadRunning,
						mt_evtAllCompressFinish,
						mt_dwFileCount,
						nWrite,
						mt_dwAddressQueue,
						dwAddressDataAreaEndAt,
						mt_pckCompressedDataPtrArrayGet);

					break;
				}

				memcpy(lpBufferToWrite, dataToWrite, lpPckIndexTableComp->dwCompressedFilesize);

				dwAddressDataAreaEndAt += lpPckIndexTableComp->dwAddressAddStep;

				free(dataToWrite);
				mt_lpFileWrite->UnmapView();
			}

			//已读入文件数+1
			nWrite++;

			pThis->freeMaxAndSubtractMemory(lpPckIndexTableComp->dwMallocSize);

			if(mt_dwFileCountOfWriteTarget == nWrite) {
				WaitForSingleObject(mt_evtAllCompressFinish, INFINITE);
				mt_dwAddressQueue = dwAddressDataAreaEndAt;

				break;
			}

		} else {

			if(!(*mt_lpbThreadRunning)) {
				//取消
				WaitForSingleObject(mt_evtAllCompressFinish, INFINITE);

				mt_dwFileCountOfWriteTarget = nWrite;
				mt_dwAddressQueue = dwAddressDataAreaEndAt;

				break;
			}

			::Sleep(10);
		}
	}

	SetEvent(mt_evtAllWriteFinish);
	return;
}


BOOL CPckClass::CreatePckFile(LPTSTR szPckFile, LPTSTR szPath)
{

	m_PckLog.PrintLogI(TEXT( TEXT_LOG_CREATE ), szPckFile);

	DWORD		dwFileCount = 0;									//文件数量, 原pck文件中的文件数
	QWORD		qwTotalFileSize = 0;								//未压缩时所有文件大小
	QWORD		dwAddress = PCK_DATA_START_AT;

	DWORD		IndexCompressedFilenameDataLengthCryptKey1 = m_PckAllInfo.lpSaveAsPckVerFunc->cPckXorKeys->IndexCompressedFilenameDataLengthCryptKey1;
	DWORD		IndexCompressedFilenameDataLengthCryptKey2 = m_PckAllInfo.lpSaveAsPckVerFunc->cPckXorKeys->IndexCompressedFilenameDataLengthCryptKey2;

	char		szPathMbsc[MAX_PATH];

	int			level = m_lpPckParams->dwCompressLevel;
	int			threadnum = m_lpPckParams->dwMTThread;

	//LOG
	m_PckLog.PrintLogI(TEXT_LOG_LEVEL_THREAD, level, threadnum);

	//开始查找文件
	LinkList<FILES_TO_COMPRESS> cFileLinkList;

	size_t nLen = lstrlen(szPath) - 1;
	if('\\' == *(szPath + nLen))*(szPath + nLen) = 0;
	BOOL	IsPatition = lstrlen(szPath) == 2 ? TRUE : FALSE;

#ifdef UNICODE
	CUcs2Ansi cU2A;
	nLen = cU2A.GetStrlen(szPath, szPathMbsc, MAX_PATH) + 1;
#else
	strcpy(szPathMbsc, szPath);
	nLen = strlen(szPathMbsc) + 1;
#endif

	m_firstFile = cFileLinkList.first();

	//遍历所有文件
	NPckClassFileDisk::EnumFile(szPathMbsc, IsPatition, dwFileCount, &cFileLinkList, qwTotalFileSize, nLen);
	if(0 == dwFileCount)return TRUE;

	//文件数写入窗口类中保存以显示进度
	mt_dwFileCount = m_lpPckParams->cVarParams.dwUIProgressUpper = dwFileCount;

	//计算大概需要多大空间qwTotalFileSize
	mt_CompressTotalFileSize = NPckClassFileDisk::GetPckFilesizeByCompressed(szPckFile, qwTotalFileSize, 0);

	PCK_ALL_INFOS	pckAllInfo;
	if(!BeforeSingleOrMultiThreadProcess(&pckAllInfo, mt_lpFileWrite, szPckFile, CREATE_ALWAYS, mt_CompressTotalFileSize, threadnum))
	{
		assert(FALSE);
		return FALSE;
	}

	if(!initCompressedDataQueue(threadnum, mt_dwFileCountOfWriteTarget = mt_dwFileCount, dwAddress))
	{
		delete mt_lpFileWrite;
		assert(FALSE);
		return FALSE;
	}

	MultiThreadInitialize(CompressThreadCreate, WriteThread, threadnum);
	dwAddress = mt_dwAddressQueue;

	//LOG
	m_PckLog.PrintLogI(TEXT_LOG_COMPRESSOK);

	//写文件索引
	pckAllInfo.dwAddressOfFilenameIndex = dwAddress;
	WritePckIndexTable(mt_lpFileWrite, mt_lpPckIndexTable, mt_dwFileCountOfWriteTarget, dwAddress);

	pckAllInfo.dwFileCount = mt_dwFileCountOfWriteTarget;
	AfterProcess(mt_lpFileWrite, pckAllInfo, dwAddress);

	delete mt_lpFileWrite;

	uninitCompressedDataQueue(threadnum);

	//LOG
	m_PckLog.PrintLogI(TEXT_LOG_WORKING_DONE);

	return TRUE;
}


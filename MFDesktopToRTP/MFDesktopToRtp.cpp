/// Filename: MFWebCamRtp.cpp
///
/// Description:
/// This file contains a C++ console application that captures the realtime video stream from a webcam using 
/// Windows Media Foundation, encodes it as H264 and then transmits it to an RTP end point using the real-time
/// communications API from Live555 (http://live555.com/).
///
/// To view the RTP feed produced by this sample the steps are:
/// 1. Download ffplay from http://ffmpeg.zeranoe.com/builds/ (the static build has a ready to go ffplay executable),
/// 2. Create a file called test.sdp with contents as below:
/// v=0
/// o = -0 0 IN IP4 127.0.0.1
/// s = No Name
/// t = 0 0
/// c = IN IP4 127.0.0.1
/// m = video 1234 RTP / AVP 96
/// a = rtpmap:96 H264 / 90000
/// a = fmtp : 96 packetization - mode = 1
/// 3. Start ffplay BEFORE running this sample:
/// ffplay -i test.sdp -x 800 -y 600 -profile:v baseline
///
/// History:
/// 07 Sep 2015	Aaron Clauson (aaron@sipsorcery.com)	Created.
///
/// License: Public
/// License for Live555: LGPL (http://live555.com/liveMedia/#license)

#include "DesktopDuplication.h"
#include "liveMedia.hh"
#include "BasicUsageEnvironment.hh"
#include "GroupsockHelper.hh"
#include <stdint.h>
#include <stdio.h>
#include <tchar.h>
#include <evr.h>
#include <mfplay.h>
#include <mfapi.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <codecapi.h>
#include "..\Common\MFUtility.h"
#include "SmartPtr.h"
#include <iostream>
#include <timeapi.h>

class MediaFoundationH264LiveSource :
	public FramedSource
{
private:
	static const int TARGET_FRAME_RATE = 60;// 5; 15; 30	// Note that this if the video device does not support this frame rate the video source reader will fail to initialise.
	static const int TARGET_AVERAGE_BIT_RATE = 30000000; // Adjusting this affects the quality of the H264 bit stream.
	const UINT64 VIDEO_FRAME_DURATION = 10 * 1000 * 1000 / TARGET_FRAME_RATE;

	bool _isInitialised = false;
	EventTriggerId eventTriggerId = 0;
	int _frameCount = 0;
	long int _lastSendAt;
	LONGLONG mTimeStamp = 0;

	CComPtr<IMFTransform> _pTransform = NULL; //< this is H264 Encoder MFT
	CComPtr<IMFAttributes> attributes;
	CComQIPtr<IMFMediaEventGenerator> eventGen;
	DWORD inputStreamID;
	DWORD outputStreamID;

	CComPtr<IMFDXGIDeviceManager> deviceManager;
	SmartPtr<cImageCapturingModuleId3d11Impl> pDesktopDuplication;

	// Set input type
	CComPtr<IMFMediaType> inputType;
	// Set output type
	CComPtr<IMFMediaType> outputType;
	CComPtr<IMFActivate> activate = NULL;
	// Find encoder
	CComHeapPtr<IMFActivate*> activateRaw;

	CRect deviceRect;

public:
	static MediaFoundationH264LiveSource* createNew(UsageEnvironment& env)
	{
		return new MediaFoundationH264LiveSource(env);
	}

	MediaFoundationH264LiveSource(UsageEnvironment& env) :
		FramedSource(env)
	{
		_lastSendAt = GetTickCount();
		eventTriggerId = envir().taskScheduler().createEventTrigger(deliverFrame0);
	}

	~MediaFoundationH264LiveSource()
	{ }

	bool isH264VideoStreamFramer() const {
		return true;
	}

	static void deliverFrame0(void* clientData) {
		((MediaFoundationH264LiveSource*)clientData)->doGetNextFrame();
	}

	bool initialise()
	{
		HRESULT hr;

		HDESK CurrentDesktop = nullptr;
		CurrentDesktop = OpenInputDesktop(0, FALSE, GENERIC_ALL);
		if (!CurrentDesktop)
		{
			// We do not have access to the desktop so request a retry
			return false;
		}

		// Attach desktop to this thread
		bool DesktopAttached = SetThreadDesktop(CurrentDesktop) != 0;
		CloseDesktop(CurrentDesktop);
		CurrentDesktop = nullptr;
		if (!DesktopAttached)
		{
			printf("SetThreadDesktop failed\n");
		}

		UINT32 activateCount = 0;

		// h264 output
		MFT_REGISTER_TYPE_INFO info = { MFMediaType_Video, MFVideoFormat_H264 };

		UINT32 flags =
			MFT_ENUM_FLAG_HARDWARE |
			MFT_ENUM_FLAG_SORTANDFILTER;

		if (!pDesktopDuplication) {

			pDesktopDuplication = (new cImageCapturingModuleId3d11Impl())->template DetachObject<cImageCapturingModuleId3d11Impl>();
		}

		if (!pDesktopDuplication->InitimageCapturingModule(deviceRect, deviceManager)) {

			return false;
		}

		hr = MFTEnumEx(
			MFT_CATEGORY_VIDEO_ENCODER,
			flags,
			NULL,
			&info,
			&activateRaw,
			&activateCount
		);
		CHECK_HR(hr, "Failed to enumerate MFTs");

		CHECK(activateCount, "No MFTs found");

		// Choose the first available encoder
		activate = activateRaw[0];

		for (UINT32 i = 0; i < activateCount; i++)
			activateRaw[i]->Release();

		// Activate
		hr = activate->ActivateObject(IID_PPV_ARGS(&_pTransform));
		CHECK_HR(hr, "Failed to activate MFT");

		// Get attributes
		hr = _pTransform->GetAttributes(&attributes);
		CHECK_HR(hr, "Failed to get MFT attributes");

#if 0
		// Get encoder name
		UINT32 nameLength = 0;
		std::wstring name;

		hr = attributes->GetStringLength(MFT_FRIENDLY_NAME_Attribute, &nameLength);
		CHECK_HR(hr, "Failed to get MFT name length");

		// IMFAttributes::GetString returns a null-terminated wide string
		name.resize(nameLength + 1);

		hr = attributes->GetString(MFT_FRIENDLY_NAME_Attribute, &name[0], name.size(), &nameLength);
		CHECK_HR(hr, "Failed to get MFT name");

		name.resize(nameLength);

		std::wcout << name << std::endl;
#endif

		// Unlock the transform for async use and get event generator
		hr = attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
		CHECK_HR(hr, "Failed to unlock MFT");

		eventGen = _pTransform;
		CHECK(eventGen, "Failed to QI for event generator");

		// Get stream IDs (expect 1 input and 1 output stream)
		hr = _pTransform->GetStreamIDs(1, &inputStreamID, 1, &outputStreamID);
		if (hr == E_NOTIMPL)
		{
			inputStreamID = 0;
			outputStreamID = 0;
			hr = S_OK;
		}
		CHECK_HR(hr, "Failed to get stream IDs");

		 // ------------------------------------------------------------------------
		// Configure hardware encoder MFT
	   // ------------------------------------------------------------------------
		CHECK_HR(_pTransform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(deviceManager.p)), "Failed to set device manager.\n");

		// Set low latency hint
		hr = attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
		CHECK_HR(hr, "Failed to set MF_LOW_LATENCY");

		hr = MFCreateMediaType(&outputType);
		CHECK_HR(hr, "Failed to create media type");

		hr = outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		CHECK_HR(hr, "Failed to set MF_MT_MAJOR_TYPE on H264 output media type");

		hr = outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
		CHECK_HR(hr, "Failed to set MF_MT_SUBTYPE on H264 output media type");

		hr = outputType->SetUINT32(MF_MT_AVG_BITRATE, TARGET_AVERAGE_BIT_RATE);
		CHECK_HR(hr, "Failed to set average bit rate on H264 output media type");

		hr = MFSetAttributeSize(outputType, MF_MT_FRAME_SIZE, deviceRect.Width(), deviceRect.Height());
		CHECK_HR(hr, "Failed to set frame size on H264 MFT out type");

		hr = MFSetAttributeRatio(outputType, MF_MT_FRAME_RATE, TARGET_FRAME_RATE, 1);
		CHECK_HR(hr, "Failed to set frame rate on H264 MFT out type");

		hr = outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
		CHECK_HR(hr, "Failed to set MF_MT_INTERLACE_MODE on H.264 encoder MFT");

		hr = outputType->SetUINT32(MF_MT_MAX_KEYFRAME_SPACING, 16);

		CHECK_HR(hr, "Failed to set key frame spacing");

		hr = outputType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
		CHECK_HR(hr, "Failed to set MF_MT_ALL_SAMPLES_INDEPENDENT on H.264 encoder MFT");

		hr = _pTransform->SetOutputType(outputStreamID, outputType, 0);
		CHECK_HR(hr, "Failed to set output media type on H.264 encoder MFT");

		hr = MFCreateMediaType(&inputType);
		CHECK_HR(hr, "Failed to create media type");

		for (DWORD i = 0;; i++)
		{
			inputType = nullptr;
			hr = _pTransform->GetInputAvailableType(inputStreamID, i, &inputType);
			CHECK_HR(hr, "Failed to get input type");

			hr = inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
			CHECK_HR(hr, "Failed to set MF_MT_MAJOR_TYPE on H264 MFT input type");

			hr = inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
			CHECK_HR(hr, "Failed to set MF_MT_SUBTYPE on H264 MFT input type");

			hr = MFSetAttributeSize(inputType, MF_MT_FRAME_SIZE, deviceRect.Width(), deviceRect.Height());
			CHECK_HR(hr, "Failed to set MF_MT_FRAME_SIZE on H264 MFT input type");

			hr = MFSetAttributeRatio(inputType, MF_MT_FRAME_RATE, TARGET_FRAME_RATE, 1);
			CHECK_HR(hr, "Failed to set MF_MT_FRAME_RATE on H264 MFT input type");

			hr = _pTransform->SetInputType(inputStreamID, inputType, 0);
			CHECK_HR(hr, "Failed to set input type");

			break;
		}

		CHECK_HR(_pTransform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, NULL), "Failed to process FLUSH command on H.264 MFT.\n");
		CHECK_HR(_pTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, NULL), "Failed to process BEGIN_STREAMING command on H.264 MFT.\n");
		CHECK_HR(_pTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, NULL), "Failed to process START_OF_STREAM command on H.264 MFT.\n");

		return true;

	done:

		printf("MediaFoundationH264LiveSource initialisation failed.\n");
		return false;
	}

	bool GenerateNextSample(MediaEventType eventType) {

		DWORD processOutputStatus = 0;
		HRESULT mftProcessOutput = S_OK;
		MFT_OUTPUT_STREAM_INFO StreamInfo;
		IMFMediaBuffer *pBuffer = NULL;
		IMFSample *mftOutSample = NULL;
		DWORD mftOutFlags;
		bool bTimeout = false;
		bool frameSent = false;

		// Create sample
		IMFSample *videoSample = nullptr;
		IMFMediaBuffer *pMediaBuffer = nullptr;

		switch (eventType)
		{
		case METransformNeedInput:
		{
			if (pDesktopDuplication->GetCurrentFrameAsVideoSample((void**)&videoSample, (void**)&pMediaBuffer, bTimeout, deviceRect, deviceRect.Width(), deviceRect.Height())) {

				if (videoSample)
				{
					_frameCount++;

					CHECK_HR(videoSample->SetSampleTime(mTimeStamp), "Error setting the video sample time.\n");
					CHECK_HR(videoSample->SetSampleDuration(VIDEO_FRAME_DURATION), "Error getting video sample duration.\n");

					// Pass the video sample to the H.264 transform.

					HRESULT hr = _pTransform->ProcessInput(inputStreamID, videoSample, 0);
					CHECK_HR(hr, "The resampler H264 ProcessInput call failed.\n");

					mTimeStamp += VIDEO_FRAME_DURATION;

					SafeRelease(&videoSample);
					SafeRelease(&pMediaBuffer);

					(pDesktopDuplication)->releaseBuffer();

					//TODO: Check any special handling needed for TIMEOUT case
					(pDesktopDuplication)->cleanUpCurrentFrameObjects();
				}
			}
		}

		break;

		case METransformHaveOutput:

		{
			CHECK_HR(_pTransform->GetOutputStatus(&mftOutFlags), "H264 MFT GetOutputStatus failed.\n");

			if (mftOutFlags == MFT_OUTPUT_STATUS_SAMPLE_READY)
			{
				MFT_OUTPUT_DATA_BUFFER _outputDataBuffer;
				memset(&_outputDataBuffer, 0, sizeof _outputDataBuffer);

				/*CHECK_HR(_pTransform->GetOutputStreamInfo(0, &StreamInfo), "Failed to get output stream info from H264 MFT.\n");

				CHECK_HR(MFCreateSample(&mftOutSample), "Failed to create MF sample.\n");
				CHECK_HR(MFCreateMemoryBuffer(StreamInfo.cbSize, &pBuffer), "Failed to create memory buffer.\n");
				CHECK_HR(mftOutSample->AddBuffer(pBuffer), "Failed to add sample to buffer.\n");
				*/
				_outputDataBuffer.dwStreamID = outputStreamID;
				_outputDataBuffer.dwStatus = 0;
				_outputDataBuffer.pEvents = nullptr;
				_outputDataBuffer.pSample = nullptr;

				mftProcessOutput = _pTransform->ProcessOutput(0, 1, &_outputDataBuffer, &processOutputStatus);

				if (mftProcessOutput != MF_E_TRANSFORM_NEED_MORE_INPUT)
				{
					if (_outputDataBuffer.pSample) {

						//CHECK_HR(_outputDataBuffer.pSample->SetSampleTime(mTimeStamp), "Error setting MFT sample time.\n");
						//CHECK_HR(_outputDataBuffer.pSample->SetSampleDuration(VIDEO_FRAME_DURATION), "Error setting MFT sample duration.\n");

						IMFMediaBuffer *buf = NULL;
						DWORD bufLength;
						CHECK_HR(_outputDataBuffer.pSample->ConvertToContiguousBuffer(&buf), "ConvertToContiguousBuffer failed.\n");
						CHECK_HR(buf->GetCurrentLength(&bufLength), "Get buffer length failed.\n");
						BYTE * rawBuffer = NULL;

#if 0
						auto now = GetTickCount();
						printf("Writing sample %i, spacing %I64dms, sample time %I64d, sample duration %I64d, sample size %i.\n", _frameCount, now - _lastSendAt, mTimeStamp, VIDEO_FRAME_DURATION, bufLength);
#endif

						fFrameSize = bufLength;
						fDurationInMicroseconds = 0;
						gettimeofday(&fPresentationTime, NULL);

						buf->Lock(&rawBuffer, NULL, NULL);
						memmove(fTo, rawBuffer, fFrameSize);

						FramedSource::afterGetting(this);

						buf->Unlock();
						SafeRelease(&buf);

						frameSent = true;
						_lastSendAt = GetTickCount();

						_outputDataBuffer.pSample->Release();
					}

					if (_outputDataBuffer.pEvents)
						_outputDataBuffer.pEvents->Release();
				}
				else if (MF_E_TRANSFORM_STREAM_CHANGE == mftProcessOutput) {
					printf("stream change event received");
				}

				//SafeRelease(&pBuffer);
				//SafeRelease(&mftOutSample);
			}
		}

		break;

		default:
			printf("unknown case");
		}

		return frameSent;

	done:

		printf("MediaFoundationH264LiveSource doGetNextFrame failed.\n");
		return false;
	}

	virtual void doGetNextFrame()
	{
		MediaEventType eventType = METransformNeedInput;
		bool frameSent = false;

		while (eventType != METransformHaveOutput && !frameSent) {

			if (!_isInitialised)
			{
				if (!initialise()) {
					printf("Video device initialisation failed, stopping.");
					return;
				}
				else {
					_isInitialised = true;
				}
			}

			if (!isCurrentlyAwaitingData()) return;

			// Get next event
			CComPtr<IMFMediaEvent> event;
			HRESULT hr = eventGen->GetEvent(0, &event);
			CHECK_HR(hr, "Failed to get next event");

			hr = event->GetType(&eventType);
			CHECK_HR(hr, "Failed to get event type");

			frameSent = GenerateNextSample(eventType);
		}

		if (!frameSent)
		{
			envir().taskScheduler().triggerEvent(eventTriggerId, this);
		}

		return;

	done:

		printf("MediaFoundationH264LiveSource doGetNextFrame failed.\n");
		envir().taskScheduler().triggerEvent(eventTriggerId, this);
	}
};

int main()
{
	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	MFStartup(MF_VERSION);

	TaskScheduler* scheduler = BasicTaskScheduler::createNew();
	UsageEnvironment* env = BasicUsageEnvironment::createNew(*scheduler);

	in_addr dstAddr = { 127, 0, 0, 1 };
	Groupsock rtpGroupsock(*env, dstAddr, 1233, 255);
	rtpGroupsock.addDestination(dstAddr, 1234, 0);
	RTPSink * rtpSink = H264VideoRTPSink::createNew(*env, &rtpGroupsock, 96);


	MediaFoundationH264LiveSource * mediaFoundationH264Source = MediaFoundationH264LiveSource::createNew(*env);
	//mediaFoundationH264Source->doGetNextFrame();
	rtpSink->startPlaying(*mediaFoundationH264Source, NULL, NULL);

	// This function call does not return.
	env->taskScheduler().doEventLoop();

	return 0;
}
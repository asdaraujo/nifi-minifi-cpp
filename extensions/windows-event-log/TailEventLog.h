/**
 * @file TailEventLog.h
 * TailEventLog class declaration
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef TAIL_EVENT_LOG_H_
#define TAIL_EVENT_LOG_H_

#include "core/Core.h"
#include "FlowFileRecord.h"
#include "core/Processor.h"
#include "core/ProcessSession.h"

namespace org {
namespace apache {
namespace nifi {
namespace minifi {
namespace processors {

#define MAX_RECORD_BUFFER_SIZE 0x10000 // 64k
char log_name[255] = "Application";

const std::string MIME_JSON = "application/json";
const std::string MIME_TEXT = "text/plain";

typedef struct {
	std::string log_source_;
	int64_t current_record_id_;
} TailState;

//! TailEventLog Class
class TailEventLog : public core::Processor
{
public:
	//! Constructor
	TailEventLog(
		std::string name,
		utils::Identifier uuid = utils::Identifier()
	) : core::Processor(name, uuid),
		logger_(logging::LoggerFactory<TailEventLog>::getLogger()),
		max_events_(1) {
	}

	//! Destructor
	virtual ~TailEventLog()
	{
	}

	//! Processor Name
	static const std::string ProcessorName;

	//! Supported Properties
	static core::Property LogSourceFileName;
	static core::Property MaxEventsPerFlowFile;
	static core::Property StateFile;
	static core::Property IncludeData;
	static core::Property IncludeStrings;
	static core::Property AutoOffsetReset;
	static core::Property MimeType;

	//! Supported Relationships
	static core::Relationship Success;

public:
	//! OnSchedule method, implemented by NiFi TailEventLog
	void onSchedule(const std::shared_ptr<core::ProcessContext> &context, const std::shared_ptr<core::ProcessSessionFactory> &sessionFactory) override;
	//! OnTrigger method, implemented by NiFi TailEventLog
	virtual void onTrigger(const std::shared_ptr<core::ProcessContext> &context, const std::shared_ptr<core::ProcessSession> &session) override;
	//! Initialize, over write by NiFi TailEventLog
	virtual void initialize(void) override;
	// recoverState
	bool recoverState();
	// storeState
	void storeState();
	void TailEventLog::parseStateFileLine(char *buf);
	std::string TailEventLog::trimRight(const std::string& s);
	std::pair<std::string, DWORD> TailEventLog::formatTextEvent(const LPSTR& strings, DWORD num_strings, io::DataStream data_stream, DWORD data_stream_length, std::map<std::string, std::string> attributes);
	std::pair<std::string, DWORD> TailEventLog::formatJsonEvent(const LPSTR& strings, DWORD num_strings, io::DataStream data_stream, DWORD data_stream_length, std::map<std::string, std::string> attributes);
	std::pair<std::string, DWORD> TailEventLog::convertToBase64(const char *buffer, DWORD length);

	static const int BUFFER_SIZE = 10240;

protected:

	virtual void notifyStop() override{
		CloseEventLog(log_handle_);
	}

	inline std::string typeToString(WORD wEventType)
	{
		switch (wEventType)
		{
		case EVENTLOG_ERROR_TYPE:
			return "Error";
		case EVENTLOG_WARNING_TYPE:
			return "Warning";
		case EVENTLOG_INFORMATION_TYPE:
			return "Information";
		case EVENTLOG_AUDIT_SUCCESS:
			return "Audit Success";
		case EVENTLOG_AUDIT_FAILURE:
			return "Audit Failure";
		default:
			return "Unknown Event";
		}
	}

	std::string getTimeStamp(const DWORD Time)
	{
		uint64_t ullTimeStamp = 0;
		uint64_t SecsTo1970 = 116444736000000000;
		SYSTEMTIME st;
		FILETIME ft, ftLocal;

		ullTimeStamp = Int32x32To64(Time, 10000000) + SecsTo1970;
		ft.dwHighDateTime = (DWORD)((ullTimeStamp >> 32) & 0xFFFFFFFF);
		ft.dwLowDateTime = (DWORD)(ullTimeStamp & 0xFFFFFFFF);

		FileTimeToLocalFileTime(&ft, &ftLocal);
		FileTimeToSystemTime(&ftLocal, &st);

		std::stringstream str;
		str.precision(2);
		str << st.wYear << "-"
			<< (st.wMonth < 10 ? "0" : "") << st.wMonth << "-" 
			<< (st.wDay < 10 ? "0" : "") << st.wDay << " " 
			<< (st.wHour < 10 ? "0" : "") << st.wHour << ":"
			<< (st.wMinute < 10 ? "0" : "") << st.wMinute << ":"
			<< (st.wSecond < 10 ? "0" : "") << st.wSecond;

		return str.str();
	}


	/**

	*/
	void LogWindowsError(void)
	{
		auto error_id = GetLastError();
		LPVOID lpMsg;

		FormatMessage(
			FORMAT_MESSAGE_ALLOCATE_BUFFER |
			FORMAT_MESSAGE_FROM_SYSTEM,
			NULL,
			error_id,
			MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
			(LPTSTR)&lpMsg,
			0, NULL);

		logger_->log_debug("Error %d: %s\n", (int)error_id, (char *)lpMsg);

	}



private:
  std::string log_source_;
  uint32_t max_events_;
  int32_t auto_offset_reset_;
  bool include_data_;
  bool include_strings_;
  std::string mime_type_;
  DWORD current_record_;
  DWORD last_record_;
  DWORD num_records_;

  std::mutex tail_event_log_mutex_;
  // File to save state
  std::string state_file_;
  // determine if state is recovered;
  bool state_recovered_;
  // map to store the state
  std::map<std::string, TailState> tail_states_;

  HANDLE log_handle_;
  // Logger
  std::shared_ptr<logging::Logger> logger_;
};
REGISTER_RESOURCE(TailEventLog, "Windows event log reader that functions as a stateful tail of the provided windows event log name");

} /* namespace processors */
} /* namespace minifi */
} /* namespace nifi */
} /* namespace apache */
} /* namespace org */

#endif

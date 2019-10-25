/**
 * @file TailEventLog.cpp
 * TailEventLog class implementation
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

#include "TailEventLog.h"
#include <vector>
#include <queue>
#include <map>
#include <set>
#include <sstream>
#include <stdio.h>
#include <string>
#include <iostream>

#include "io/DataStream.h"
#include "core/ProcessContext.h"
#include "core/ProcessSession.h"
#include "utils/base64.h"

#include "rapidjson/document.h"
#include "rapidjson/ostreamwrapper.h"
#include "rapidjson/istreamwrapper.h"
#include "rapidjson/writer.h"


namespace org {
namespace apache {
namespace nifi {
namespace minifi {
namespace processors {

const std::string TailEventLog::ProcessorName("TailEventLog");

core::Relationship TailEventLog::Success("success", "All files, containing log events, are routed to success");

core::Property TailEventLog::LogSourceFileName(
	core::PropertyBuilder::createProperty("Log Source")->
	isRequired(true)->
	withDefaultValue("Application")->
	withDescription("Log Source from which to read events")->
	build());

core::Property TailEventLog::MaxEventsPerFlowFile(
	core::PropertyBuilder::createProperty("Max Events Per FlowFile")->
	isRequired(true)->
	withDefaultValue<uint32_t>(1)->
	withDescription("Events per flow file")->
	build());

core::Property TailEventLog::IncludeData(
	core::PropertyBuilder::createProperty("Include Data?")->
	isRequired(true)->
	withDefaultValue<bool>(true)->
	withDescription("If true it will include the message Data payload in the flowfile content. Note that the Event's Data and Strings fields have different contents.")->
	build());

core::Property TailEventLog::IncludeStrings(
	core::PropertyBuilder::createProperty("Include Strings?")->
	isRequired(true)->
	withDefaultValue<bool>(true)->
	withDescription("If true it will include the message Strings payload in the flowfile content. Note that the Event's Data and Strings fields have different contents.")->
	build());

core::Property TailEventLog::MimeType(
	core::PropertyBuilder::createProperty("Mime Type")->
	isRequired(true)->
	withDefaultValue("text/plain")->
	withDescription("Defines the format of the flowfile contents. Valid values: text/plain (default) or application/json.")->
	build());

core::Property TailEventLog::AutoOffsetReset(
	core::PropertyBuilder::createProperty("Auto Offset Reset")->
	isRequired(false)->
	withDefaultValue<int32_t>(-1)->
	withDescription("Determines from which event log to start reading if the State File doesn't exist."
		            "Valid values are: 0 (beginning on the Log Source), -1 (end of the Log Source - default), or "
		            "any positive value to start at a specific event offset. If the State File exists "
	                "this parameter is ignored")->
	build());

core::Property TailEventLog::StateFile(
	core::PropertyBuilder::createProperty("State File")->
	isRequired(true)->
	withDefaultValue("TailEventLogState")->
	withDescription("Specifies the file that should be used for storing state about"
	                " what data has been ingested so that upon restart NiFi can resume from where it left off" )->
	build());


void TailEventLog::initialize() {
  //! Set the supported properties
  std::set<core::Property> properties;
  properties.insert(LogSourceFileName);
  properties.insert(MaxEventsPerFlowFile);
  properties.insert(IncludeData);
  properties.insert(IncludeStrings);
  properties.insert(MimeType);
  properties.insert(AutoOffsetReset);
  properties.insert(StateFile);
  setSupportedProperties(properties);
  //! Set the supported relationships
  std::set<core::Relationship> relationships;
  relationships.insert(Success);
  setSupportedRelationships(relationships);
}

void TailEventLog::onSchedule(const std::shared_ptr<core::ProcessContext> &context, const std::shared_ptr<core::ProcessSessionFactory> &sessionFactory) {
	std::lock_guard<std::mutex> tail_lock(tail_event_log_mutex_);
    std::string value;
    if (context->getProperty(LogSourceFileName.getName(), value)) {
        log_source_ = value;
    }
	if (context->getProperty(MaxEventsPerFlowFile.getName(), value)) {
		core::Property::StringToInt(value, max_events_);
	}
	if (context->getProperty(AutoOffsetReset.getName(), value)) {
		core::Property::StringToInt(value, auto_offset_reset_);
	}
	if (context->getProperty(IncludeData.getName(), value)) {
		include_data_ = (value.compare("true") == 0);
	}
	if (context->getProperty(IncludeStrings.getName(), value)) {
		include_strings_ = (value.compare("true") == 0);
	}
	if (context->getProperty(MimeType.getName(), value)) {
		if (value.compare(MIME_JSON) != 0 && value.compare(MIME_TEXT) != 0) {
			logger_->log_warn("Invalid MIME type: %s. Using %s instead.", value, MIME_TEXT);
			mime_type_ = MIME_TEXT;
		} else {
			mime_type_ = value;
		}
	}

    log_handle_ = OpenEventLog(NULL, log_source_.c_str());

    logger_->log_trace("TailEventLog configured to tail %s", log_source_);

    // can perform these in notifyStop, but this has the same outcome
    tail_states_.clear();
    state_recovered_ = false;
}

std::pair<std::string, DWORD> TailEventLog::convertToBase64(const char *buffer, DWORD length) {
	if (length == 0) {
		return std::pair<std::string, DWORD>("", 0);
	} else {
		char* b64_out = nullptr;
		DWORD b64_len = Curl_base64_encode((const char*)buffer, length, &b64_out);

		if (b64_out == nullptr) {
			return std::pair<std::string, DWORD>("", 0);
		}
		else {
			return std::pair<std::string, DWORD>(std::string(b64_out, b64_len), b64_len);
		}
	}
}

std::pair<std::string, DWORD> TailEventLog::formatTextEvent(const LPSTR& strings, DWORD num_strings, io::DataStream data_stream, DWORD data_stream_length, std::map<std::string, std::string> attributes) {
	std::string buffer;
	DWORD len = 0;
	LPSTR strings_ = strings;
	const std::string new_line = "\n";
	const std::string attr_sep = ": ";
	const std::string str_prefix = "String ";
	const std::string data_header = "Data: ";

	buffer.clear();
	for (auto attr : attributes) {
		buffer.append(attr.first);
		buffer.append(attr_sep);
		buffer.append(attr.second);
		buffer.append(new_line);
		len += attr.first.length() + attr_sep.length() + attr.second.length() + new_line.length();
	}

	if (include_strings_) {
		for (auto i = 0; i < num_strings; i++) {
			auto data_size_ = std::strlen(strings_);
			std::string num = std::to_string(i + 1);
			buffer.append(str_prefix);
			buffer.append(num);
			buffer.append(attr_sep);
			buffer.append((LPSTR)strings_, 0, data_size_);
			buffer.append(new_line);
			len += str_prefix.length() + num.length() + attr_sep.length() + data_size_ + new_line.length();
			strings_ = (LPSTR)((LPBYTE)strings_ + data_size_ + 1);
		}
	}

	if (include_data_ && data_stream_length > 0) {
		std::string data;
		DWORD data_len;
		std:tie(data, data_len) = convertToBase64((const char*)data_stream.getBuffer(), data_stream.getSize());

		auto data_size_ = data_stream.getSize();
		buffer.append(data_header);
		buffer.append(data.c_str(), 0, data_len);
		len += data_header.length() + data_len;
	}
	return std::pair<std::string, DWORD>(buffer, len);
}

std::pair<std::string, DWORD> TailEventLog::formatJsonEvent(const LPSTR& strings, DWORD num_strings, io::DataStream data_stream, DWORD data_stream_length, std::map<std::string, std::string> attributes) {
	rapidjson::Document entities(rapidjson::kObjectType);
	rapidjson::Document::AllocatorType& alloc = entities.GetAllocator();

	for (auto attr : attributes) {
		rapidjson::Value key(attr.first.c_str(), alloc);
		rapidjson::Value value(attr.second.c_str(), alloc);
		entities.AddMember(key.Move(), value.Move(), alloc);
	}

	LPSTR strings_ = strings;
	if (include_strings_) {
		rapidjson::Document string_array(rapidjson::kArrayType);
		for (auto i = 0; i < num_strings; i++) {
			auto data_size_ = std::strlen(strings_);
			rapidjson::Value value;
			value.SetString((const char*)strings_, data_size_, alloc);
			string_array.PushBack(value.Move(), alloc);
			strings_ = (LPSTR)((LPBYTE)strings_ + data_size_ + 1);
		}
		entities.AddMember("strings", string_array.Move(), alloc);
	}

	if (include_data_) {
		std::string data;
		DWORD len;
		std:tie(data, len) = convertToBase64((const char*)data_stream.getBuffer(), data_stream.getSize());

		rapidjson::Value value;
		value.SetString(data.c_str(), len, alloc);
		entities.AddMember("data", value.Move(), alloc);
	}

	rapidjson::StringBuffer buffer;
	buffer.Clear();
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	entities.Accept(writer);
	buffer.Put('\n');
	return std::pair<std::string, DWORD>(buffer.GetString(), buffer.GetSize());
}

void TailEventLog::onTrigger(const std::shared_ptr<core::ProcessContext> &context, const std::shared_ptr<core::ProcessSession> &session) {
	std::lock_guard<std::mutex> tail_lock(tail_event_log_mutex_);

	struct WriteCallback : public OutputStreamCallback {
		WriteCallback(LPSTR data, DWORD data_length)
			: data_(data), data_length_(data_length) {
		}

		int64_t process(std::shared_ptr<io::BaseStream> stream) {
			return stream->writeData((uint8_t*)data_, data_length_);
		}

		LPSTR data_;
		DWORD data_length_;
	};

	std::string st_file;
	if (context->getProperty(StateFile.getName(), st_file)) {
		state_file_ = st_file + "." + getUUIDStr();
	}
	if (!this->state_recovered_) {
		// recover the state if we have not done so
		this->recoverState();

		// check if state already contains this log source and add it if not
		if (tail_states_.count(log_source_) == 0) {
			tail_states_[log_source_] = TailState{ log_source_, auto_offset_reset_ };
		}

		this->state_recovered_ = true;
	}

	if (log_handle_ == nullptr) {
		logger_->log_debug("Handle could not be created for %s", log_source_);
	}

	BYTE buffer[MAX_RECORD_BUFFER_SIZE];

	EVENTLOGRECORD *event_record = (EVENTLOGRECORD*)&buffer;
	logger_->log_trace("Buffer base: %d", event_record);

	DWORD bytes_to_read = 0, min_bytes = 0;
	
	GetOldestEventLogRecord(log_handle_, &current_record_);
	logger_->log_trace("Oldest record: %d", current_record_);
	GetNumberOfEventLogRecords(log_handle_, &num_records_);
	logger_->log_trace("Number of records: %d", num_records_);
	last_record_ = current_record_ + num_records_ - 1;

	int64_t checkpoint_record_id = tail_states_[log_source_].current_record_id_;
	if (checkpoint_record_id > current_record_) {
		current_record_ = checkpoint_record_id;
	} else if (checkpoint_record_id < 0) {
		current_record_ = last_record_ + 1;
	}

	std::string data = "";
	DWORD data_length = 0;
	DWORD event_counter = 0;
	DWORD batch_counter = 0;
	DWORD batches = 0;
	std::shared_ptr<FlowFileRecord> flowFile = nullptr;
	while (current_record_ <= last_record_) {
		DWORD cr = current_record_;
		DWORD lr = last_record_;
		DWORD rem = lr - cr;
		if (ReadEventLog(log_handle_, EVENTLOG_FORWARDS_READ | EVENTLOG_SEEK_READ, current_record_, event_record, MAX_RECORD_BUFFER_SIZE, &bytes_to_read, &min_bytes))
		{
			if (bytes_to_read == 0) {
				logger_->log_debug("Yielding");
				context->yield();
				break;
			}
			while (bytes_to_read > 0)
			{
				LPSTR source = (LPSTR)((LPBYTE)event_record + sizeof(EVENTLOGRECORD));
				LPSTR computer_name = (LPSTR)((LPBYTE)event_record + sizeof(EVENTLOGRECORD) + strlen(source) + 1);

				if (flowFile == nullptr) {
					flowFile = std::static_pointer_cast<FlowFileRecord>(session->create());
				}
				if (flowFile == nullptr)
					return;

				std::map<std::string, std::string> attributes;
				attributes["source"] = source;
				attributes["record_number"] = std::to_string(event_record->RecordNumber);
				attributes["computer_name"] = computer_name;
				attributes["length"] = std::to_string(event_record->Length);
				attributes["record_number"] = std::to_string(event_record->RecordNumber);
				attributes["time_generated"] = std::to_string(event_record->TimeGenerated);
				attributes["time_generated_str"] = getTimeStamp(event_record->TimeGenerated);
				attributes["time_written"] = std::to_string(event_record->TimeWritten);
				attributes["time_written_str"] = getTimeStamp(event_record->TimeWritten);
				attributes["event_id"] = std::to_string(event_record->EventID);
				attributes["num_strings"] = std::to_string(event_record->NumStrings);
				attributes["event_category"] = std::to_string(event_record->EventCategory);
				attributes["string_offset"] = std::to_string(event_record->StringOffset);
				attributes["user_sid"] = std::to_string(event_record->UserSidLength);
				attributes["user_sid_offset"] = std::to_string(event_record->UserSidOffset);
				attributes["original_data_length"] = std::to_string(event_record->DataLength);
				attributes["data_offset"] = std::to_string(event_record->DataOffset);
				attributes["event_type"] = typeToString(event_record->EventType);

				io::DataStream stream((const uint8_t*)((LPBYTE)event_record + event_record->DataOffset), event_record->DataLength);
				LPSTR string_start = (LPSTR)((LPBYTE)event_record + event_record->StringOffset);
				std::string scratch;
				DWORD scratch_length;
				if (mime_type_.compare(MIME_TEXT) == 0) {
					std::tie(scratch, scratch_length) = formatTextEvent(string_start, event_record->NumStrings, stream, event_record->DataLength, attributes);
				} else { // MIME_JSON
					std::tie(scratch, scratch_length) = formatJsonEvent(string_start, event_record->NumStrings, stream, event_record->DataLength, attributes);
				}
				data.append(scratch, 0, scratch_length);
				data_length += scratch_length;

				event_counter += 1;
				batch_counter += 1;
				if (batch_counter >= max_events_) {
					batches += 1;
					if (max_events_ == 1) {
						// flowfile attributes only make sense for 1 event per file
						for (auto attr : attributes) {
							flowFile->addAttribute("event." + attr.first, attr.second);
						}
					}
					flowFile->addAttribute("mime.type", mime_type_);
					session->write(flowFile, &WriteCallback((LPSTR)data.c_str(), data_length));
					session->transfer(flowFile, Success);
					flowFile = nullptr;
					data.clear();
					data_length = 0;
					batch_counter = 0;

					if (event_counter % 1000 == 0) {
						std::cout << "Record " << current_record_ << "\n";
						session->commit();
						// update state
						tail_states_[log_source_].current_record_id_ = current_record_;
						// store the state
						storeState();
					}
				}
				bytes_to_read -= event_record->Length;
				logger_->log_trace("Bytes to read: %d", bytes_to_read);
				current_record_++;
				if (bytes_to_read > 0) {
					event_record = (EVENTLOGRECORD *)
						((LPBYTE)event_record + event_record->Length);
				}
			}

			event_record = (EVENTLOGRECORD *)&buffer;
		} else {
			LogWindowsError();
			logger_->log_trace("Yielding due to error");
			context->yield();
			break;
		}
	}
	if (batch_counter > 0) {
	    session->write(flowFile, &WriteCallback((LPSTR)data.c_str(), data_length));
		session->transfer(flowFile, Success);
		session->commit();
		// update state
		tail_states_[log_source_].current_record_id_ = current_record_;
		// store the state
		storeState();
	}


	logger_->log_trace("Yielding due to end of data");
	context->yield();
}

std::string TailEventLog::trimRight(const std::string& s) {
	return org::apache::nifi::minifi::utils::StringUtils::trimRight(s);
}

void TailEventLog::parseStateFileLine(char *buf) {
	char *line = buf;

	logger_->log_trace("Received line %s", buf);

	// skip leading blanks
	while ((line[0] == ' ') || (line[0] == '\t'))
		++line;

	// skip comments or blank lines
	char first = line[0];
	if ((first == '\0') || (first == '#') || (first == '\r') || (first == '\n') || (first == '=')) {
		return;
	}

	// find position of the equal sign
	char *equal = strchr(line, '=');
	if (equal == NULL) {
		return;
	}

	// get key
	equal[0] = '\0';
	std::string key = trimRight(line);

	// skip spaces after the equal sign
	equal++;
	while ((equal[0] == ' ') || (equal[0] == '\t'))
		++equal;

	// skip lines with no values after the equal sign
	first = equal[0];
	if ((first == '\0') || (first == '\r') || (first == '\n')) {
		return;
	}

	// get value
	std::string value = trimRight(equal);

	const int64_t position = std::stoull(value);
	logger_->log_debug("Received log source = %s, record id = %d", key, position);
	tail_states_[key] = TailState{ key, position };

	return;
}

bool TailEventLog::recoverState() {
	std::ifstream file(state_file_.c_str(), std::ifstream::in);
	bool file_is_good = file.good();
	if (!file_is_good) {
		return false;
	}
	tail_states_.clear();
	char buf[BUFFER_SIZE];
	for (file.getline(buf, BUFFER_SIZE); file.good(); file.getline(buf, BUFFER_SIZE)) {
		parseStateFileLine(buf);
	}
	return true;
}

void TailEventLog::storeState() {
	std::ofstream file(state_file_.c_str());
	if (!file.is_open()) {
		logger_->log_error("store state file failed %s", state_file_);
		return;
	}
	for (const auto &state : tail_states_) {
		file << state.first << "=" << state.second.current_record_id_ << "\n";
	}
	file.close();
}

} /* namespace processors */
} /* namespace minifi */
} /* namespace nifi */
} /* namespace apache */
} /* namespace org */
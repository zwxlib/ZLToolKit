/*
 * logger.h
 *
 *  Created on: 2016年8月3日
 *      Author: xzl
 */

#ifndef UTIL_LOGGER_H_
#define UTIL_LOGGER_H_

#include <iostream>
#include <fstream>
#include <sstream>
#include <deque>
#include <map>
#include <ctime>
#include <string.h>
#include <condition_variable>
#include <cstdlib>
#include <thread>
#include <memory>
#include <mutex>
#include <time.h>
using namespace std;

namespace ZL {
namespace Util {

enum LogLevel {
	LTrace = 0,
	LDebug ,
	LInfo ,
	LWarn ,
	LError ,
	LFatal ,
};
static const char
*LogLevelStr[]=	{"trace",
				"debug",
				"info",
				"warn",
				"error",
				"fatal"};

#define CLEAR_COLOR "\033[0m"
#define UNDERLINE "\033[4m"

static const char
*COLOR[6][2] = {{ "\033[44;37m", "\033[34m" },
				{"\033[42;37m", "\033[32m" },
				{ "\033[46;37m", "\033[36m" },
				{"\033[43;37m", "\033[33m" },
				{ "\033[45;37m", "\033[35m" },
				{"\033[41;37m", "\033[31m" } };

class Logger;
class LogWriter;
class LogChannel;
class LogInfo;
class LogInfoMaker;

typedef shared_ptr<LogInfo> LogInfo_ptr;
class LogChannel {
public:
	LogChannel(const string& name,
			LogLevel level = LDebug,
			const char* timeFormat = "%Y-%m-%d %H:%M:%S") :
			_name(name),
			_level(level),
			_timeFormat(timeFormat) {
	}
	virtual ~LogChannel() {}
	virtual void write(const LogInfo_ptr & stream)=0;
	const string &name() const {
		return _name;
	}
	LogLevel level() const {
		return _level;
	}
	const string &timeFormat() const {
		return _timeFormat;
	}
	void setLevel(LogLevel level) {
		_level = level;
	}
	void setDateFormat(const char* format) {
		_timeFormat = format;
	}
protected:
	string _name;
	LogLevel _level;
	string _timeFormat;
};

class LogWriter {
public:
	LogWriter() {}
	virtual ~LogWriter() {}
	virtual void write(const LogInfo_ptr &stream) =0;
};

class Logger {
public:
	friend class LogWriter;
	friend class AsyncLogWriter;
	static Logger& instance() {
		static Logger *logger(new Logger());
		return *logger;
	}
	void add(const shared_ptr<LogChannel> &&channel) {
		channels[channel->name()]=channel;
	}
	void remove(const string& name) {
		auto it = channels.find(name);
		if (it != channels.end()) {
			channels.erase(it);
		}
	}
	void setWriter(const shared_ptr<LogWriter> &&_writer) {
		if (_writer) {
			this->writer = _writer;
		}
	}
	void write(const LogInfo_ptr &stream) {
		if (!writer) {
			for (auto &chn : Logger::instance().channels) {
				chn.second->write(stream);
			}
			return;
		}
		writer->write(stream);
	}
protected:
	Logger(){}
	~Logger(){}
	// Non-copyable and non-movable
	Logger(const Logger&); // = delete;
	Logger(Logger&&); // = delete;
	Logger& operator=(const Logger&); // = delete;
	Logger& operator=(Logger&&); // = delete;

	map<string, shared_ptr<LogChannel> > channels;
	shared_ptr<LogWriter> writer;
};

class LogInfo {
public:
	friend class LogInfoMaker;
	void format(ostream& ost,
			const char *timeFormat = "%Y-%m-%d %H:%M:%S",
			bool enableColor = true) {
		ost << file << " " << line << "\r\n ";
		if (enableColor) {
			ost << COLOR[level][1];
		}
		if (timeFormat) {
			ost << print(toLocal(ts), timeFormat);
		}
		ost << " [" << LogLevelStr[level] << "] ";
		ost << function << " ";
		ost << message.str();
		if (enableColor) {
			ost << CLEAR_COLOR;
		}
		ost.flush();
	}

	LogLevel getLevel() const {
		return level;
	}

private:
	LogInfo(LogLevel _level,
			const char* _file,
			const char* _function,
			int _line) :
			level(_level),
			line(_line),
			file(_file),
			function(_function),
			ts(::time(NULL)) {
	}
	std::string print(const std::tm& dt, const char* fmt) {
#if defined(WIN32)
		// BOGUS hack done for VS2012: C++11 non-conformant since it SHOULD take a "const struct tm* "
		// ref. C++11 standard: ISO/IEC 14882:2011, � 27.7.1,
		std::ostringstream oss;
		oss << std::put_time(const_cast<std::tm*>(&dt), fmt);
		return oss.str();

#else    // LINUX
		const size_t size = 1024;
		char buffer[size];
		auto success = std::strftime(buffer, size, fmt, &dt);
		if (0 == success)
			return string(fmt);
		return buffer;
#endif
	}

	std::tm toLocal(const std::time_t& time) {
		std::tm tm_snapshot;
#if defined(WIN32)
		localtime_s(&tm_snapshot, &time); // thread-safe?
#else
		localtime_r(&time, &tm_snapshot); // POSIX
#endif
		return tm_snapshot;
	}
	LogLevel level;
	int line;
	string file;
	string function;
	time_t ts;
	ostringstream message;
};

class LogInfoMaker {
public:
	LogInfoMaker(LogLevel level,
			const char* file,
			const char* function,
			int line) :
			logInfo(new LogInfo(level, file, function, line)) {
	}
	LogInfoMaker(LogInfoMaker &&that) {
		this->logInfo = that.logInfo;
		that.logInfo.reset();
	}
	LogInfoMaker(const LogInfoMaker &that) {
		this->logInfo = that.logInfo;
		(const_cast<LogInfoMaker &>(that)).logInfo.reset();
	}
	~LogInfoMaker() {
		*this << endl;
	}
	template<typename T>
	LogInfoMaker& operator <<(const T& data) {
		if (!logInfo) {
			return *this;
		}
		logInfo->message << data;
		return *this;
	}
	LogInfoMaker& operator <<(ostream&(*f)(ostream&)) {
		if (!logInfo) {
			return *this;
		}
		logInfo->message << f;
		Logger::instance().write(logInfo);
		logInfo.reset();
		return *this;
	}
	void clear(){
		logInfo.reset();
	}
private:
	LogInfo_ptr logInfo;
};

class AsyncLogWriter: public LogWriter {
public:
	AsyncLogWriter() :exit_flag(false), _thread([this]() {this->run();}) {}
	virtual ~AsyncLogWriter() {
		exit_flag = true;
		cond.notify_one();
		_thread.join();
		flush();
	}
	virtual void write(const LogInfo_ptr &stream) {
		unique_lock<recursive_mutex> lock(_mutex);
		_pending.push_back(stream);
		cond.notify_one();
	}
protected:
	void run() {
		while (!exit_flag) {
			unique_lock<recursive_mutex> lock(_mutex);
			cond.wait(lock);
			flush();
		}
	}
	inline void flush() {
		while (!_pending.empty()) {
			auto &next = _pending.front();
			realWrite(next);
			_pending.pop_front();
		}
	}
	inline void realWrite(const LogInfo_ptr &stream) {
		for (auto &chn : Logger::instance().channels) {
			chn.second->write(stream);
		}
	}
	bool exit_flag;
	thread _thread;
	deque<LogInfo_ptr > _pending;
	condition_variable_any cond;
	recursive_mutex _mutex;
};

class ConsoleChannel: public LogChannel {
public:
	ConsoleChannel(const string& name,
			LogLevel level = LDebug,
			const char* timeFormat = "%Y-%m-%d %H:%M:%S"):
			LogChannel(name, level, timeFormat){
	}
	virtual ~ConsoleChannel() {
	}
	void write(const LogInfo_ptr &logInfo) {
		if (level() > logInfo->getLevel()) {
			return;
		}
		logInfo->format(std::cout, timeFormat().c_str(), true);
	}
};

class FileChannel: public LogChannel {
public:
	FileChannel(const string& name,
			const string& path,
			LogLevel level = LDebug,
			const char* timeFormat = "%Y-%m-%d %H:%M:%S") :
			LogChannel(name, level, timeFormat),
			_path(path) {
	}
	virtual ~FileChannel() {
		close();
	}
	virtual void write(const shared_ptr<LogInfo> &stream) {
		if (level() > stream->getLevel()) {
			return;
		}
		if (!_fstream.is_open()) {
			open();
		}
		stream->format(_fstream, timeFormat().c_str(), false);
	}
	void setPath(const string& path) {
		_path = path;
		open();
	}
	const string &path() const {
		return _path;
	}
protected:
	virtual void open() {
		// Ensure a path was set
		if (_path.empty()) {
			throw runtime_error("Log file path must be set.");
		}
		// Open the file stream
		_fstream.close();
		_fstream.open(_path.c_str(), ios::out | ios::app);
		// Throw on failure
		if (!_fstream.is_open()) {
			throw runtime_error("Failed to open log file: " + _path);
		}
	}
	virtual void close() {
		_fstream.close();
	}
	ofstream _fstream;
	string _path;
};


#define TraceL LogInfoMaker(LTrace, __FILE__,__FUNCTION__, __LINE__)
#define DebugL LogInfoMaker(LDebug, __FILE__,__FUNCTION__, __LINE__)
#define InfoL LogInfoMaker(LInfo, __FILE__,__FUNCTION__, __LINE__)
#define WarnL LogInfoMaker(LWarn,__FILE__, __FUNCTION__, __LINE__)
#define ErrorL LogInfoMaker(LError,__FILE__, __FUNCTION__, __LINE__)
#define FatalL LogInfoMaker(LFatal,__FILE__, __FUNCTION__, __LINE__)


} /* namespace util */
} /* namespace ZL */

#endif /* UTIL_LOGGER_H_ */

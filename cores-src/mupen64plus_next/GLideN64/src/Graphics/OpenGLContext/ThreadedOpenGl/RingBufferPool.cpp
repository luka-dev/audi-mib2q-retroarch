#include "RingBufferPool.h"
#include <memory>
#include <algorithm>
#include <sstream>
#include <Log.h>

namespace opengl {


PoolBufferPointer::PoolBufferPointer() :
	m_offset(0),
	m_size(0),
	m_realSize(0),
	m_isValid(false)
{

}

PoolBufferPointer::PoolBufferPointer(size_t _offset, size_t _size, size_t _realSize, bool _isValid) :
	m_offset(_offset),
	m_size(_size),
	m_realSize(_realSize),
	m_isValid(_isValid)
{
}

PoolBufferPointer::PoolBufferPointer(const PoolBufferPointer& other) :
	m_offset(other.m_offset),
	m_size(other.m_size),
	m_realSize(other.m_realSize),
	m_isValid(other.m_isValid)
{
}

PoolBufferPointer& PoolBufferPointer::operator=(const PoolBufferPointer& other)
{
	m_offset = other.m_offset;
	m_size = other.m_size;
	m_realSize = other.m_realSize;
	m_isValid = other.m_isValid;
	return *this;
}

bool PoolBufferPointer::isValid() const
{
	return m_isValid;
}

size_t PoolBufferPointer::getSize() const
{
	return m_size;
}

RingBufferPool::RingBufferPool(size_t _poolSize) :
	m_poolBuffer(m_startBufferPoolSize, 0),
	m_inUseStartOffset(0),
	m_inUseEndOffset(0),
	m_full(false),
	m_maxBufferPoolSize(_poolSize)
{

}

PoolBufferPointer RingBufferPool::createPoolBuffer(const char* _buffer, size_t _bufferSize)
{
	const size_t byteAlignment = 8;
	size_t realBufferSize;
	size_t startOffset;
	std::unique_lock<std::mutex> lock(m_mutex);

	if (_bufferSize == 0)
		return PoolBufferPointer();
	if (_buffer == nullptr || _bufferSize > m_maxBufferPoolSize ||
		_bufferSize > static_cast<size_t>(-1) - (byteAlignment - 1)) {
		std::stringstream errorString;
		errorString << " Attempted to create buffer of invalid size, size="
			<< _bufferSize << ", max_size=" << m_maxBufferPoolSize;
		LOG(LOG_ERROR, errorString.str().c_str());
		throw std::runtime_error(errorString.str().c_str());
	}

	realBufferSize = (_bufferSize + byteAlignment - 1) & ~(byteAlignment - 1);

	for (;;) {
		/* std::vector::resize invalidates every pointer held by the GL consumer.
		 * It is therefore legal only when the ring is completely empty. */
		if (realBufferSize > m_poolBuffer.size()) {
			if (!m_full && m_inUseStartOffset == m_inUseEndOffset) {
				std::stringstream logString;
				logString << " Increasing buffer size from " << m_poolBuffer.size()
					<< " to " << realBufferSize;
				LOG(LOG_VERBOSE, logString.str().c_str());
				m_poolBuffer.resize(realBufferSize);
				m_inUseStartOffset = 0;
				m_inUseEndOffset = 0;
			} else {
				m_condition.wait(lock);
				continue;
			}
		}

		if (m_full) {
			m_condition.wait(lock);
			continue;
		}

		if (m_inUseEndOffset >= m_inUseStartOffset) {
			const size_t tailSpace = m_poolBuffer.size() - m_inUseEndOffset;
			if (realBufferSize <= tailSpace)
				startOffset = m_inUseEndOffset;
			else if (realBufferSize <= m_inUseStartOffset)
				startOffset = 0;
			else {
				m_condition.wait(lock);
				continue;
			}
		} else {
			const size_t middleSpace = m_inUseStartOffset - m_inUseEndOffset;
			if (realBufferSize <= middleSpace)
				startOffset = m_inUseEndOffset;
			else {
				m_condition.wait(lock);
				continue;
			}
		}

		/* Keep both the range proof and the copy under the same mutex. The old
		 * implementation published new offsets before memcpy() without holding
		 * m_mutex, allowing the consumer to recycle that range concurrently. */
		if (startOffset > m_poolBuffer.size() ||
			realBufferSize > m_poolBuffer.size() - startOffset ||
			_bufferSize > m_poolBuffer.size() - startOffset) {
			LOG(LOG_ERROR, " RingBufferPool internal bounds violation");
			throw std::runtime_error("RingBufferPool internal bounds violation");
		}
		std::copy_n(_buffer, _bufferSize, m_poolBuffer.data() + startOffset);

		m_inUseEndOffset = (startOffset + realBufferSize) % m_poolBuffer.size();
		m_full = m_inUseEndOffset == m_inUseStartOffset;
		return PoolBufferPointer(startOffset, _bufferSize, realBufferSize, true);
	}
}

const char* RingBufferPool::getBufferFromPool(PoolBufferPointer _poolBufferPointer)
{
	if (!_poolBufferPointer.isValid()) {
		return nullptr;
	} else {
		std::unique_lock<std::mutex> lock(m_mutex);
		if (_poolBufferPointer.m_offset > m_poolBuffer.size() ||
			_poolBufferPointer.m_size > m_poolBuffer.size() - _poolBufferPointer.m_offset) {
			LOG(LOG_ERROR, " RingBufferPool read bounds violation");
			return nullptr;
		}
		return m_poolBuffer.data() + _poolBufferPointer.m_offset;
	}
}

void RingBufferPool::removeBufferFromPool(PoolBufferPointer _poolBufferPointer)
{
	if (_poolBufferPointer.isValid()) {
		std::unique_lock<std::mutex> lock(m_mutex);
		/* Allocations never straddle the physical end of the vector. When the
		 * producer wraps, an unused tail gap may remain; FIFO consumption is
		 * allowed to jump from that gap to the next allocation at offset zero. */
		const bool wrappedTailGap = _poolBufferPointer.m_offset == 0 &&
			m_inUseStartOffset > m_inUseEndOffset;
		if ((_poolBufferPointer.m_offset != m_inUseStartOffset && !wrappedTailGap) ||
			_poolBufferPointer.m_realSize > m_poolBuffer.size() - _poolBufferPointer.m_offset) {
			LOG(LOG_ERROR, " RingBufferPool release order/bounds violation");
			throw std::runtime_error("RingBufferPool release order/bounds violation");
		}
		m_inUseStartOffset = (_poolBufferPointer.m_offset +
			_poolBufferPointer.m_realSize) % m_poolBuffer.size();
		m_full = false;
		m_condition.notify_all();
	}
}

}

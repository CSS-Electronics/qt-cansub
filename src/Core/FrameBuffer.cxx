#include <algorithm>
#include <cstring>
#include <memory>

#include "FrameBuffer.hxx"

FrameBuffer::FrameBuffer(std::size_t capacity) :
    data(std::make_unique<std::byte[]>(capacity)), capacity(capacity)
{

}

bool FrameBuffer::append(const std::byte* src, std::size_t length)
{
    if (capacity - writePosition < length)
    {
        /* Tail too small, attempt to reclaim postfix */
        compact();
        if (capacity - writePosition < length)
        {
            return false;
        }
    }
    std::memcpy(data.get() + writePosition, src, length);
    writePosition += length;
    return true;
}

std::size_t FrameBuffer::available() const
{
    return writePosition - readPosition;
}

const std::byte* FrameBuffer::peek() const
{
    return data.get() + readPosition;
}

void FrameBuffer::consume(std::size_t n)
{
    readPosition = std::clamp(readPosition + n, readPosition, writePosition);
}

void FrameBuffer::recycle()
{
    if (readPosition == writePosition)
    {
        readPosition = writePosition = 0;
    }
}

void FrameBuffer::clear()
{
    readPosition = writePosition = 0;
}

void FrameBuffer::compact()
{
    if (readPosition == 0)
    {
        return;
    }

    const std::size_t n = writePosition - readPosition;
    std::memmove(data.get(), data.get() + readPosition, n);
    readPosition = 0;
    writePosition = n;
}

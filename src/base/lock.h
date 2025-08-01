#ifndef BASE_LOCK_H
#define BASE_LOCK_H

#include <mutex>

// Enable thread safety attributes only with clang.
// The attributes can be safely erased when compiling with other compilers.
#if defined(__clang__) && (!defined(SWIG))
#define THREAD_ANNOTATION_ATTRIBUTE__(x) __attribute__((x))
#else
#define THREAD_ANNOTATION_ATTRIBUTE__(x) // no-op
#endif

#define CAPABILITY(x) \
	THREAD_ANNOTATION_ATTRIBUTE__(capability(x))

#define SCOPED_CAPABILITY \
	THREAD_ANNOTATION_ATTRIBUTE__(scoped_lockable)

#define GUARDED_BY(x) \
	THREAD_ANNOTATION_ATTRIBUTE__(guarded_by(x))

#define PT_GUARDED_BY(x) \
	THREAD_ANNOTATION_ATTRIBUTE__(pt_guarded_by(x))

#define ACQUIRED_BEFORE(...) \
	THREAD_ANNOTATION_ATTRIBUTE__(acquired_before(__VA_ARGS__))

#define ACQUIRED_AFTER(...) \
	THREAD_ANNOTATION_ATTRIBUTE__(acquired_after(__VA_ARGS__))

#define REQUIRES(...) \
	THREAD_ANNOTATION_ATTRIBUTE__(requires_capability(__VA_ARGS__))

#define REQUIRES_SHARED(...) \
	THREAD_ANNOTATION_ATTRIBUTE__(requires_shared_capability(__VA_ARGS__))

#define ACQUIRE(...) \
	THREAD_ANNOTATION_ATTRIBUTE__(acquire_capability(__VA_ARGS__))

#define ACQUIRE_SHARED(...) \
	THREAD_ANNOTATION_ATTRIBUTE__(acquire_shared_capability(__VA_ARGS__))

#define RELEASE(...) \
	THREAD_ANNOTATION_ATTRIBUTE__(release_capability(__VA_ARGS__))

#define RELEASE_SHARED(...) \
	THREAD_ANNOTATION_ATTRIBUTE__(release_shared_capability(__VA_ARGS__))

#define RELEASE_GENERIC(...) \
	THREAD_ANNOTATION_ATTRIBUTE__(release_generic_capability(__VA_ARGS__))

#define TRY_ACQUIRE(...) \
	THREAD_ANNOTATION_ATTRIBUTE__(try_acquire_capability(__VA_ARGS__))

#define TRY_ACQUIRE_SHARED(...) \
	THREAD_ANNOTATION_ATTRIBUTE__(try_acquire_shared_capability(__VA_ARGS__))

#define EXCLUDES(...) \
	THREAD_ANNOTATION_ATTRIBUTE__(locks_excluded(__VA_ARGS__))

#define ASSERT_CAPABILITY(x) \
	THREAD_ANNOTATION_ATTRIBUTE__(assert_capability(x))

#define ASSERT_SHARED_CAPABILITY(x) \
	THREAD_ANNOTATION_ATTRIBUTE__(assert_shared_capability(x))

#define RETURN_CAPABILITY(x) \
	THREAD_ANNOTATION_ATTRIBUTE__(lock_returned(x))

#define NO_THREAD_SAFETY_ANALYSIS \
	THREAD_ANNOTATION_ATTRIBUTE__(no_thread_safety_analysis)

/**
 * @defgroup Locks
 * @see Threads
 */

/**
 * Wrapper for `std::mutex`.
 *
 * @ingroup Locks
 *
 * @remark This wrapper is only necessary because the clang thread-safety attributes
 * are not available for `std::mutex` except when explicitly using libc++.
 */
class CAPABILITY("mutex") CmutexLock
{
public:
	CmutexLock() = default;

	void lock() ACQUIRE()
	{
		m_Mutex.lock();
	}

	void unlock() RELEASE()
	{
		m_Mutex.unlock();
	}

	// To support negative capabilities, otherwise EXCLUDES(m_Lock) must be used instead of REQUIRES(!m_Lock)
	const CmutexLock &operator!() const { return *this; }

private:
	std::mutex m_Mutex;
};

/**
 * RAII-style wrapper for owning a `CLock`.
 *
 * @ingroup Locks
 * 
 * @remark This wrapper is only necessary because the clang thread-safety attributes
 * are not available for `std::lock_guard` except when explicitly using libc++.
 */
class SCOPED_CAPABILITY CLockScope
{
public:
	explicit CLockScope(CmutexLock &Lock) ACQUIRE(Lock, m_Lock) :
		m_Lock(Lock)
	{
		m_Lock.lock();
	}

	~CLockScope() RELEASE() REQUIRES(m_Lock)
	{
		m_Lock.unlock();
	}

private:
	CmutexLock &m_Lock;
};

#endif

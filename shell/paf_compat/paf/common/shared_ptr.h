/*
	ScePaf-RE paf/common/shared_ptr.h with one GCC fix: the friend
	declaration's template parameter must not shadow the class's.
*/

#ifndef _VDSUITE_USER_PAF_COMMON_SHARED_PTR_H
#define _VDSUITE_USER_PAF_COMMON_SHARED_PTR_H

#include <stdint.h>
#include <paf/thread/atomic.h>

namespace paf {
	namespace common {

		template <typename T> class SharedPtr;

		namespace detail {

			template<class T>
			class RefCounter
			{

				template <typename U> friend class SharedPtr;

			public:

				typedef void(*Deleter)(T *p);

				RefCounter(T *ptr, Deleter deleter) : m_ptr(ptr), m_counter(1), m_weak_counter(1)
				{
					m_deleter = deleter;
				}

				RefCounter(T *ptr) : m_ptr(ptr), m_counter(1), m_weak_counter(1), m_deleter(default_deleter)
				{

				}

				static void default_deleter(T *ptr)
				{
					delete ptr;
				}

				int32_t inc_counter()
				{
					return thread::atomic::inc32(&m_counter);
				}

				int32_t dec_counter()
				{
					return thread::atomic::dec32(&m_counter);
				}

				int32_t inc_weak_counter()
				{
					return thread::atomic::inc32(&m_weak_counter);
				}

				int32_t dec_weak_counter()
				{
					return thread::atomic::dec32(&m_weak_counter);
				}

				void destroy()
				{
					m_deleter(m_ptr);
				}

			private:

				T* m_ptr;
				int32_t m_counter;
				int32_t m_weak_counter;
				Deleter m_deleter;
			};
		}

		template <typename T>
		class SharedPtr
		{
		public:

			explicit SharedPtr()
			{
				m_ptr = NULL;
				m_ref_counter = NULL;
			}

			explicit SharedPtr(T* ptr)
			{
				init(ptr);
			}

			template <typename U>
			SharedPtr(const SharedPtr<U>& r) : m_ptr(r.m_ptr), m_ref_counter(r.m_ref_counter)
			{
				add_ref();
			}

			SharedPtr(const SharedPtr<T>& r) : m_ptr(r.m_ptr), m_ref_counter(r.m_ref_counter)
			{
				add_ref();
			}

			~SharedPtr()
			{
				release();
			}

			void add_ref()
			{
				if (m_ptr != NULL)
				{
					m_ref_counter->inc_counter();
				}
			}

			void assign(T* p, detail::RefCounter<T> *ref_counter)
			{
				if (p != m_ptr)
				{
					release();
					m_ptr = p;
					m_ref_counter = ref_counter;
					add_ref();
				}
			}

			template <typename U>
			SharedPtr<T>& operator=(const SharedPtr<U>& r)
			{
				assign(r.m_ptr, r.m_ref_counter);
				return *this;
			}

			SharedPtr<T>& operator=(const SharedPtr<T>& r)
			{
				assign(r.m_ptr, r.m_ref_counter);
				return *this;
			}

			void release()
			{
				if ((m_ptr != NULL) && (m_ref_counter->dec_counter() == 1))
				{
					m_ref_counter->destroy();
					if (m_ref_counter->dec_weak_counter() == 1)
					{
						delete m_ref_counter;
					}
				}
			}

			void reset()
			{
				release();
				m_ptr = NULL;
				m_ref_counter = NULL;
			}

			T* get() const
			{
				return m_ptr;
			}

			T& operator*() const
			{
				return *m_ptr;
			}

			T* operator->() const
			{
				return m_ptr;
			}

			int32_t use_count() const
			{
				return m_ptr ? m_ref_counter->m_counter : 0;
			}

		private:

			void init(T* p)
			{
				m_ptr = p;
				if (m_ptr == NULL)
				{
					m_ref_counter = NULL;
				}
				else
				{
					m_ref_counter = new detail::RefCounter<T>(m_ptr);
				}
			}

			T* m_ptr;
			detail::RefCounter<T> *m_ref_counter;
		};

		template <typename T>
		class WeakPtr
		{
		public:

			template <typename U>
			WeakPtr(const SharedPtr<U>& r) : m_ref_counter(r.m_ref_counter)
			{
				add_ref();
			}

			WeakPtr(const SharedPtr<T>& r) : m_ref_counter(r.m_ref_counter)
			{
				add_ref();
			}

			~WeakPtr()
			{
				release();
			}

			void add_ref()
			{
				if (m_ref_counter != NULL)
				{
					m_ref_counter->inc_counter();
				}
			}

			void release()
			{
				if ((m_ref_counter != NULL) && (m_ref_counter->dec_weak_counter() == 1))
				{
					delete m_ref_counter;
				}
			}

			void reset()
			{
				release();
				m_ref_counter = NULL;
			}

			T* get() const
			{
				return m_ref_counter->m_ptr;
			}

			T& operator*() const
			{
				return *m_ref_counter->m_ptr;
			}

			T* operator->() const
			{
				return m_ref_counter->m_ptr;
			}

			int32_t use_count() const
			{
				return m_ref_counter ? m_ref_counter->m_weak_counter : 0;
			}

		private:

			detail::RefCounter<T> *m_ref_counter;
		};
	}
}

#endif /* _VDSUITE_USER_PAF_COMMON_SHARED_PTR_H */

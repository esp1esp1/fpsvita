/*
	ScePaf-RE paf/versatileparam/versatileparam.h with two forward
	declarations so it builds without the full File and ui headers.
*/

#ifndef _VDSUITE_USER_PAF_VERSATILEPARAM_VERSATILEPARAM_H
#define _VDSUITE_USER_PAF_VERSATILEPARAM_VERSATILEPARAM_H

#include <stddef.h>
#include <paf/std/list>
#include <paf/std/string>
#include <paf/math/math.h>
#include <paf/common/shared_ptr.h>
#include <paf/versatileparam/idparam.h>
#include <paf/versatileparam/resourceparam.h>

/* Forward declarations to avoid needing full headers here */
namespace paf { class File; namespace ui { class Widget; } }

typedef void* IdParamParamHandle;
typedef void* TextureParamHandle;

namespace paf {

	class ParamBase;
	class VersatileParam;
	class Plugin;

	namespace cxml {

		class Element;
	}

	class VisualParam
	{
	public:

		enum
		{
			SERIALIZER = 1,
			VISUALIZER = 2
		};

		enum
		{
			WRITE_TAG_NORMAL = 0,
			WRITE_TAG_BEGIN = 1,
			WRITE_TAG_END = 2
		};

		class ParamBase
		{
		public:

			ParamBase(VisualParam *visual_param, string const& title) : m_visual_param(visual_param), m_title(title)
			{

			}

			virtual ~ParamBase()
			{

			}

			virtual bool IsDirty()
			{
				return false;
			}

			virtual void Draw()
			{

			}

		protected:

			VisualParam *m_visual_param;
			string m_title;
		};

		template <typename T>
		class ParamWithDirtyFlag : public ParamBase
		{
		public:

			ParamWithDirtyFlag(VisualParam *visual_param, string const& title, T *var) : ParamBase(visual_param, title), m_var(var), m_prev_value(*var), m_curr_value(*var)
			{

			}

			virtual ~ParamWithDirtyFlag()
			{

			}

			virtual bool IsDirty()
			{
				bool ret = (m_curr_value != m_prev_value);
				if (ret)
				{
					*m_var = m_curr_value;
				}
				else
				{
					m_curr_value = *m_var;
				}
				m_prev_value = m_curr_value;
				return ret;
			}

		protected:

			T *m_var;
			T m_prev_value;
			T m_curr_value;
		};

		template <typename T>
		class MinMaxParam : public ParamWithDirtyFlag<T>
		{
		public:

			MinMaxParam(VisualParam *visual_param, string const& title, T *var, T const& min_value, T const& max_value) : ParamWithDirtyFlag<T>(visual_param, title, var), m_min(min_value), m_max(max_value), m_org_value(*var)
			{

			}

			virtual ~MinMaxParam()
			{

			}

		protected:

			T m_min;
			T m_max;
			T m_org_value;
		};

		class BoolParam : public ParamWithDirtyFlag<bool>
		{
		public:

			BoolParam(VisualParam *visual_param, string const& title, bool *var) : ParamWithDirtyFlag<bool>(visual_param, title, var), m_org_value(*var)
			{

			}

			virtual ~BoolParam()
			{

			}

			virtual void Draw();

		protected:

			bool *m_var;
			bool m_org_value;
		};

		class FloatParam : public MinMaxParam<float>
		{
		public:

			FloatParam(VisualParam *visual_param, string const& title, float *var, float min_value, float max_value) : MinMaxParam<float>(visual_param, title, var, min_value, max_value)
			{

			}

			virtual ~FloatParam()
			{

			}

			virtual void Draw();
		};

		class IntParam : public MinMaxParam<int32_t>
		{
		public:

			IntParam(VisualParam *visual_param, string const& title, int32_t *var, int32_t min_value, int32_t max_value) : MinMaxParam<int32_t>(visual_param, title, var, min_value, max_value)
			{

			}

			virtual ~IntParam()
			{

			}

			virtual void Draw();
		};

		class Vec2Param : public MinMaxParam<math::v2>
		{
		public:

			Vec2Param(VisualParam *visual_param, string const& title, math::v2 *var, math::v2 const& min_value, math::v2 const& max_value) : MinMaxParam<math::v2>(visual_param, title, var, min_value, max_value)
			{

			}

			virtual ~Vec2Param()
			{

			}

			virtual void Draw();
		};

		class Vec3Param : public MinMaxParam<math::v3>
		{
		public:

			Vec3Param(VisualParam *visual_param, string const& title, math::v3 *var, math::v3 const& min_value, math::v3 const& max_value) : MinMaxParam<math::v3>(visual_param, title, var, min_value, max_value)
			{

			}

			virtual ~Vec3Param()
			{

			}

			virtual void Draw();
		};

		class Vec4Param : public MinMaxParam<math::v4>
		{
		public:

			Vec4Param(VisualParam *visual_param, string const& title, math::v4 *var, math::v4 const& min_value, math::v4 const& max_value) : MinMaxParam<math::v4>(visual_param, title, var, min_value, max_value)
			{

			}

			virtual ~Vec4Param()
			{

			}

			virtual void Draw();
		};

		class IdParamParam : public ParamBase
		{
		public:

			IdParamParam(VisualParam *visual_param, string const& title, IDParam *var) : ParamBase(visual_param, title), m_var(var), m_org_value(*var)
			{

			}

			virtual ~IdParamParam()
			{

			}

			virtual void Draw();

		protected:

			IDParam *m_var;
			IDParam m_org_value;
		};

		class TextureParam : public ParamBase
		{
		public:

			TextureParam(VisualParam *visual_param, string const& title, IDParam *var) : ParamBase(visual_param, title), m_var(var), m_org_value(*var)
			{

			}

			virtual ~TextureParam()
			{

			}

			virtual void Draw();

		protected:

			IDParam *m_var;
			IDParam m_org_value;
		};

		VisualParam(VersatileParam *versatile_param, Plugin *plugin);

		~VisualParam();

		void AddParam(const char *title, bool *var, int32_t visual_type);

		void AddParam(const char *title, int32_t *var, int32_t min_value, int32_t max_value, int32_t visual_type);

		void AddParam(const char *title, float *var, float min_value, float max_value, int32_t visual_type);

		void AddParam(const char *title, math::v2 *var, math::v2 const& min_value, math::v2 const& max_value, int32_t visual_type);

		void AddParam(const char *title, math::v3 *var, math::v3 const& min_value, math::v3 const& max_value, int32_t visual_type);

		void AddParam(const char *title, math::v4 *var, math::v4 const& min_value, math::v4 const& max_value, int32_t visual_type);

		void AddParam(const char *title, IDParam *var);

		void AddTextureParam(const char *title, IDParam *var);

		void Draw();

		bool GetDirty()
		{
			return m_dirty;
		}

		void SetDirty(bool flag)
		{
			m_dirty = flag;
		}

		void SetIDName(IDParam *id);

		void SetIdParamParam(IdParamParamHandle handle, IDParam const& value);

		void SetTextureParam(TextureParamHandle handle, IDParam *value);

		void SetParent(ui::Widget *parent)
		{

		}

	private:

		VersatileParam *m_versatile_param;
		Plugin *m_plugin;
		IDParam m_id;
		bool m_dirty;
		list<ParamBase*> m_param_list;
	};

	class VersatileParam
	{
	public:

		static const char *ParamName();

		VersatileParam();

		virtual const char *GetParamName() const;

		virtual ~VersatileParam();

		virtual int32_t Set()
		{
			return 0;
		}

		virtual int32_t Get()
		{
			return 0;
		}

		virtual int32_t Create(int32_t mode);

		virtual int32_t Destroy(int32_t mode);

		virtual int32_t OnUpdate();

		virtual void OnDraw();

		virtual int32_t AttachPlugin(Plugin *plugin)
		{
			m_plugin = plugin;
			return 0;
		}

		virtual int32_t Read(cxml::Element *elem);

		virtual int32_t Write(common::SharedPtr<File> fp, uint32_t type, uint32_t indent);

		int32_t DestroyAll()
		{
			return Destroy(0xffffffff);
		}

		void SetID(IDParam const& id)
		{
			m_id = id;
		}

		void SetID(const char *id)
		{
			m_id = id;
		}

	protected:

		ResourceParam *m_serial;
		Plugin *m_plugin;
		IDParam m_id;
		cxml::Element *m_node;

		__declspec (dllimport) static const char *m_param_name;
	};

}

#endif /* _VDSUITE_USER_PAF_VERSATILEPARAM_VERSATILEPARAM_H */
#pragma once

#include "OloEngine/Core/Base.h"
#include <memory>
#include <type_traits>
#include <typeinfo>
#include <vector>
#include <cstring>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// Type descriptor system - equivalent to choc::value::Type
	class ValueType
	{
	public:
		enum class Kind
		{
			Void,
			Float32,
			Float64,
			Int32,
			Int64,
			Bool,
			Array
		};

		ValueType() = default;
		ValueType(Kind kind) : m_kind(kind) {}
		ValueType(Kind elementKind, sizet arraySize) : m_kind(Kind::Array), m_elementType(std::make_unique<ValueType>(elementKind)), m_arraySize(arraySize) {}
		
		// Copy constructor
		ValueType(const ValueType& other) : m_kind(other.m_kind), m_arraySize(other.m_arraySize)
		{
			if (other.m_elementType)
				m_elementType = std::make_unique<ValueType>(*other.m_elementType);
		}
		
		// Move constructor
		ValueType(ValueType&& other) noexcept : m_kind(other.m_kind), m_elementType(std::move(other.m_elementType)), m_arraySize(other.m_arraySize)
		{
			other.m_kind = Kind::Void;
			other.m_arraySize = 0;
		}
		
		// Copy assignment
		ValueType& operator=(const ValueType& other)
		{
			if (this != &other)
			{
				m_kind = other.m_kind;
				m_arraySize = other.m_arraySize;
				if (other.m_elementType)
					m_elementType = std::make_unique<ValueType>(*other.m_elementType);
				else
					m_elementType.reset();
			}
			return *this;
		}
		
		// Move assignment
		ValueType& operator=(ValueType&& other) noexcept
		{
			if (this != &other)
			{
				m_kind = other.m_kind;
				m_elementType = std::move(other.m_elementType);
				m_arraySize = other.m_arraySize;
				other.m_kind = Kind::Void;
				other.m_arraySize = 0;
			}
			return *this;
		}

		// Static factory methods
		template<typename T>
		static ValueType CreatePrimitive()
		{
			if constexpr (std::is_same_v<T, f32>) return ValueType(Kind::Float32);
			else if constexpr (std::is_same_v<T, f64>) return ValueType(Kind::Float64);
			else if constexpr (std::is_same_v<T, i32>) return ValueType(Kind::Int32);
			else if constexpr (std::is_same_v<T, i64>) return ValueType(Kind::Int64);
			else if constexpr (std::is_same_v<T, bool>) return ValueType(Kind::Bool);
			else return ValueType(Kind::Void);
		}

		template<typename T>
		static ValueType CreateArray(sizet size)
		{
			return ValueType(CreatePrimitive<T>().m_kind, size);
		}

		Kind GetKind() const { return m_kind; }
		bool IsArray() const { return m_kind == Kind::Array; }
		bool IsPrimitive() const { return m_kind != Kind::Array && m_kind != Kind::Void; }
		
		const ValueType& GetElementType() const 
		{ 
			OLO_CORE_ASSERT(IsArray() && m_elementType); 
			return *m_elementType; 
		}
		
		sizet GetArraySize() const 
		{ 
			OLO_CORE_ASSERT(IsArray()); 
			return m_arraySize; 
		}

		sizet GetSizeInBytes() const
		{
			switch (m_kind)
			{
				case Kind::Float32: return sizeof(f32);
				case Kind::Float64: return sizeof(f64);
				case Kind::Int32: return sizeof(i32);
				case Kind::Int64: return sizeof(i64);
				case Kind::Bool: return sizeof(bool);
				case Kind::Array: return GetElementType().GetSizeInBytes() * m_arraySize;
				default: return 0;
			}
		}

	private:
		Kind m_kind = Kind::Void;
		std::unique_ptr<ValueType> m_elementType;
		sizet m_arraySize = 0;
	};

	//==============================================================================
	/// Dynamic value storage - equivalent to choc::value::Value
	class Value
	{
	public:
		Value() = default;
		
		template<typename T>
		explicit Value(T&& value)
		{
			SetValue(std::forward<T>(value));
		}

		Value(const Value& other) { CopyFrom(other); }
		Value& operator=(const Value& other) { CopyFrom(other); return *this; }
		
		Value(Value&& other) noexcept { MoveFrom(std::move(other)); }
		Value& operator=(Value&& other) noexcept { MoveFrom(std::move(other)); return *this; }

		~Value() { Clear(); }

		template<typename T>
		void SetValue(T value)
		{
			Clear();
			m_type = ValueType::CreatePrimitive<T>();
			m_size = sizeof(T);
			m_data = std::malloc(m_size);
			std::memcpy(m_data, &value, m_size);
			m_ownsData = true;
		}

		template<typename T>
		void SetArray(const std::vector<T>& values)
		{
			Clear();
			m_type = ValueType::CreateArray<T>(values.size());
			m_size = sizeof(T) * values.size();
			m_data = std::malloc(m_size);
			std::memcpy(m_data, values.data(), m_size);
			m_ownsData = true;
		}

		void Clear()
		{
			if (m_ownsData && m_data)
			{
				std::free(m_data);
			}
			m_data = nullptr;
			m_size = 0;
			m_ownsData = false;
			m_type = ValueType();
		}

		const ValueType& GetType() const { return m_type; }
		void* GetRawData() const { return m_data; }
		sizet GetSize() const { return m_size; }

		template<typename T>
		T GetValue() const
		{
			OLO_CORE_ASSERT(m_data && GetType().GetKind() == ValueType::CreatePrimitive<T>().GetKind());
			return *static_cast<T*>(m_data);
		}

		template<typename T>
		T* GetArray() const
		{
			OLO_CORE_ASSERT(m_data && GetType().IsArray());
			return static_cast<T*>(m_data);
		}

	private:
		ValueType m_type;
		void* m_data = nullptr;
		sizet m_size = 0;
		bool m_ownsData = false;

		void CopyFrom(const Value& other)
		{
			Clear();
			m_type = other.m_type;
			m_size = other.m_size;
			if (other.m_data && m_size > 0)
			{
				m_data = std::malloc(m_size);
				std::memcpy(m_data, other.m_data, m_size);
				m_ownsData = true;
			}
		}

		void MoveFrom(Value&& other)
		{
			Clear();
			m_type = std::move(other.m_type);
			m_data = other.m_data;
			m_size = other.m_size;
			m_ownsData = other.m_ownsData;
			
			other.m_data = nullptr;
			other.m_size = 0;
			other.m_ownsData = false;
			other.m_type = ValueType();
		}
	};

	//==============================================================================
	/// Value view for efficient access - equivalent to choc::value::ValueView
	class ValueView
	{
	public:
		ValueView() = default;
		
		// Construct view from external data (non-owning)
		ValueView(const ValueType& type, void* data, void* /* unused for compatibility */) 
			: m_type(type), m_data(data) {}

		// Construct view from a Value
		explicit ValueView(Value& value) 
			: m_type(value.GetType()), m_data(value.GetRawData()) {}

		ValueView& operator=(const Value& value)
		{
			// Copy data to our viewed location (if we're viewing external data)
			if (m_data && value.GetRawData() && m_type.GetSizeInBytes() == value.GetType().GetSizeInBytes())
			{
				std::memcpy(m_data, value.GetRawData(), m_type.GetSizeInBytes());
			}
			return *this;
		}

		template<typename T>
		ValueView& operator=(T value)
		{
			OLO_CORE_ASSERT(m_data && m_type.GetKind() == ValueType::CreatePrimitive<T>().GetKind());
			*static_cast<T*>(m_data) = value;
			return *this;
		}

		const ValueType& GetType() const { return m_type; }
		void* GetRawData() const { return m_data; }

		template<typename T>
		T GetValue() const
		{
			OLO_CORE_ASSERT(m_data && m_type.GetKind() == ValueType::CreatePrimitive<T>().GetKind());
			return *static_cast<T*>(m_data);
		}

		template<typename T>
		T* GetArray() const
		{
			OLO_CORE_ASSERT(m_data && m_type.IsArray());
			return static_cast<T*>(m_data);
		}

		bool IsArray() const { return m_type.IsArray(); }
		sizet Size() const { return m_type.IsArray() ? m_type.GetArraySize() : 1; }

		// Array access
		ValueView operator[](sizet index) const
		{
			OLO_CORE_ASSERT(IsArray() && index < Size());
			const ValueType& elementType = m_type.GetElementType();
			void* elementData = static_cast<char*>(m_data) + (index * elementType.GetSizeInBytes());
			return ValueView(elementType, elementData, nullptr);
		}

	private:
		ValueType m_type;
		void* m_data = nullptr;
	};

} // namespace OloEngine::Audio::SoundGraph
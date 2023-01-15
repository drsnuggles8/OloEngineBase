#pragma once

#include "OloEngine/Core/Base.h"

#include <string>

namespace OloEngine
{
	class Texture
	{
	public:
		virtual ~Texture() = default;

		[[nodiscard("Store this!")]] virtual uint32_t GetWidth() const = 0;
		[[nodiscard("Store this!")]] virtual uint32_t GetHeight() const = 0;
		[[nodiscard("Store this!")]] virtual uint32_t GetRendererID() const = 0;
		[[nodiscard("Store this!")]] virtual const std::string& GetPath() const = 0;

		virtual void SetData(void* data, uint32_t size) = 0;

		virtual void Bind(uint32_t slot) const = 0;

		[[nodiscard("Store this!")]] virtual bool IsLoaded() const = 0;

		virtual bool operator==(const Texture& other) const = 0;
	};

	class Texture2D : public Texture
	{
	public:
		static Ref<Texture2D> Create(uint32_t width, uint32_t height);
		static Ref<Texture2D> Create(const std::string& path);
	};

	class TextureArray
	{
	public:
		virtual ~TextureArray() = default;

		[[nodiscard("Store this!")]] virtual std::vector <uint32_t> GetWidths() const = 0;
		[[nodiscard("Store this!")]] virtual std::vector <uint32_t> GetHeights() const = 0;
		[[nodiscard("Store this!")]] virtual uint32_t GetLayers() const = 0;
		[[nodiscard("Store this!")]] virtual uint32_t GetRendererID() const = 0;
		[[nodiscard("Store this!")]] virtual const std::vector<std::string>& GetPaths() const = 0;

		virtual void SetLayerData(void* data, uint32_t size, uint32_t layer) = 0;

		virtual void Bind(const uint32_t slot) const = 0;
		virtual void BindLayer(const uint32_t slot, const uint32_t layer) const = 0;

		[[nodiscard("Store this!")]] virtual bool IsLoaded() const = 0;

		virtual bool operator==(const Texture& other) const = 0;
	};

	class Texture2DArray : public TextureArray
	{
	public:
		static Ref<Texture2DArray> Create(const std::vector<uint32_t> widths, const std::vector<uint32_t> heights);
		static Ref<Texture2DArray> Create(const std::vector<std::string>& paths);
	};
}

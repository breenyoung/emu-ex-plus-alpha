/*  This file is part of Imagine.

	Imagine is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	Imagine is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Imagine.  If not, see <http://www.gnu.org/licenses/> */

#include <algorithm>
#include <imagine/gfx/Gfx.hh>
#include <imagine/gfx/Texture.hh>
#include <imagine/util/ScopeGuard.hh>
#include "private.hh"
#ifdef __ANDROID__
#include "../../base/android/android.hh"
#include "android/GraphicBufferStorage.hh"
#include "android/SurfaceTextureStorage.hh"
#endif

#ifndef GL_TEXTURE_SWIZZLE_R
#define GL_TEXTURE_SWIZZLE_R 0x8E42
#endif

#ifndef GL_TEXTURE_SWIZZLE_G
#define GL_TEXTURE_SWIZZLE_G 0x8E43
#endif

#ifndef GL_TEXTURE_SWIZZLE_B
#define GL_TEXTURE_SWIZZLE_B 0x8E44
#endif

#ifndef GL_TEXTURE_SWIZZLE_A
#define GL_TEXTURE_SWIZZLE_A 0x8E45
#endif

#ifndef GL_MAP_WRITE_BIT
#define GL_MAP_WRITE_BIT 0x0002
#endif

#ifndef GL_MAP_INVALIDATE_BUFFER_BIT
#define GL_MAP_INVALIDATE_BUFFER_BIT 0x0008
#endif

namespace Gfx
{

static GLuint samplerNames = 0; // used when separate sampler objects not supported
static GLTextureSampler currSampler;

static TextureSampler defaultClampSampler;
static TextureSampler defaultNearestMipClampSampler;
static TextureSampler defaultNoMipClampSampler;
static TextureSampler defaultNoLinearNoMipClampSampler;
static TextureSampler defaultRepeatSampler;
static TextureSampler defaultNearestMipRepeatSampler;

static constexpr uint TEXTURE_PBOS = 1;
static GLuint texturePBO[TEXTURE_PBOS]{};
static uint texturePBOIdx = 0;

static uint makeUnpackAlignment(ptrsize addr)
{
	// find best alignment with lower 3 bits
	const uint map[]
	{
		8, 1, 2, 1, 4, 1, 2, 1
	};
	return map[addr & 7];
}

static uint unpackAlignForAddrAndPitch(void *srcAddr, uint pitch)
{
	uint alignmentForAddr = makeUnpackAlignment((ptrsize)srcAddr);
	uint alignmentForPitch = makeUnpackAlignment(pitch);
	if(alignmentForAddr < alignmentForPitch)
	{
		/*logMsg("using lowest alignment of address %p (%d) and pitch %d (%d)",
			srcAddr, alignmentForAddr, pitch, alignmentForPitch);*/
	}
	return std::min(alignmentForPitch, alignmentForAddr);
}

static GLenum makeGLDataType(const PixelFormatDesc &format)
{
	switch(format.id)
	{
		case PIXEL_RGBA8888:
		case PIXEL_BGRA8888:
		#if !defined CONFIG_GFX_OPENGL_ES
			return GL_UNSIGNED_INT_8_8_8_8_REV;
		#endif
		case PIXEL_ARGB8888:
		case PIXEL_ABGR8888:
		#if !defined CONFIG_GFX_OPENGL_ES
			return GL_UNSIGNED_INT_8_8_8_8;
		#endif
		case PIXEL_RGB888:
		case PIXEL_BGR888:
		case PIXEL_I8:
		case PIXEL_IA88:
		case PIXEL_A8:
			return GL_UNSIGNED_BYTE;
		case PIXEL_RGB565:
			return GL_UNSIGNED_SHORT_5_6_5;
		case PIXEL_ARGB1555:
			return GL_UNSIGNED_SHORT_5_5_5_1;
		case PIXEL_ARGB4444:
			return GL_UNSIGNED_SHORT_4_4_4_4;
		#if !defined CONFIG_GFX_OPENGL_ES
		case PIXEL_BGRA4444:
			return GL_UNSIGNED_SHORT_4_4_4_4_REV;
		case PIXEL_ABGR1555:
			return GL_UNSIGNED_SHORT_1_5_5_5_REV;
		#endif
		default: bug_branch("%d", format.id); return 0;
	}
}

static GLenum makeGLFormat(const PixelFormatDesc &format)
{
	switch(format.id)
	{
		case PIXEL_I8:
			return luminanceFormat;
		case PIXEL_IA88:
			return luminanceAlphaFormat;
		case PIXEL_A8:
			return alphaFormat;
		case PIXEL_RGB888:
		case PIXEL_RGB565:
			return GL_RGB;
		case PIXEL_RGBA8888:
		case PIXEL_ARGB8888:
		case PIXEL_ARGB1555:
		case PIXEL_ARGB4444:
			return GL_RGBA;
		#if !defined CONFIG_GFX_OPENGL_ES
		case PIXEL_BGR888:
			assert(supportBGRPixels);
			return GL_BGR;
		case PIXEL_ABGR8888:
		case PIXEL_BGRA8888:
		case PIXEL_BGRA4444:
		case PIXEL_ABGR1555:
			assert(supportBGRPixels);
			return GL_BGRA;
		#endif
		default: bug_branch("%d", format.id); return 0;
	}
}

static int makeGLESInternalFormat(const PixelFormatDesc &format)
{
	if(format.id == PIXEL_BGRA8888) // Apple's BGRA extension loosens the internalformat match requirement
		return bgrInternalFormat;
	else return makeGLFormat(format); // OpenGL ES manual states internalformat always equals format
}

static int makeGLSizedInternalFormat(const PixelFormatDesc &format)
{
	switch(format.id)
	{
		case PIXEL_BGRA8888:
		case PIXEL_ARGB8888:
		case PIXEL_ABGR8888:
		case PIXEL_RGBA8888:
			return GL_RGBA8;
		case PIXEL_RGB888:
		case PIXEL_BGR888:
			return GL_RGB8;
		case PIXEL_RGB565:
			#if defined CONFIG_GFX_OPENGL_ES
			return GL_RGB565;
			#else
			return GL_RGB5;
			#endif
		case PIXEL_ABGR1555:
		case PIXEL_ARGB1555:
			return GL_RGB5_A1;
		case PIXEL_ARGB4444:
		case PIXEL_BGRA4444:
			return GL_RGBA4;
		case PIXEL_I8:
			return luminanceInternalFormat;
		case PIXEL_IA88:
			return luminanceAlphaInternalFormat;
		case PIXEL_A8:
			return alphaInternalFormat;
		default: bug_branch("%d", format.id); return 0;
	}
}

static int makeGLInternalFormat(const PixelFormatDesc &format)
{
	return Config::Gfx::OPENGL_ES ? makeGLESInternalFormat(format)
		: makeGLSizedInternalFormat(format);
}

static uint typeForPixelFormat(const PixelFormatDesc &format)
{
	return (format.id == PIXEL_A8) ? TEX_2D_1 :
		(format.id == PIXEL_IA88) ? TEX_2D_2 :
		TEX_2D_4;
}

static void setTexParameteriImpl(GLenum target, GLenum pname, GLint param, const char *pnameStr)
{
	glTexParameteri(target, pname, param);
	if(handleGLErrorsVerbose([pnameStr](GLenum, const char *err) { logErr("%s in glTexParameteri with %s", err, pnameStr); }))
		logWarn("error in glTexParameteri with param %d", (int)param);
}

#define setTexParameteri(target, pname, param) setTexParameteriImpl(target, pname, param, #pname);

static void setSamplerParameteriImpl(GLuint sampler, GLenum pname, GLint param, const char *pnameStr)
{
	glSamplerParameteri(sampler, pname, param);
	if(handleGLErrorsVerbose([pnameStr](GLenum, const char *err) { logErr("%s in glSamplerParameteri with %s", err, pnameStr); }))
		logWarn("error in glSamplerParameteri with param %d", (int)param);
}

#define setSamplerParameteri(sampler, pname, param) setSamplerParameteriImpl(sampler, pname, param, #pname);

static GLint makeMinFilter(bool linearFiltering, MipFilterMode mipFiltering)
{
	switch(mipFiltering)
	{
		case MIP_FILTER_NONE: return linearFiltering ? GL_LINEAR : GL_NEAREST;
		case MIP_FILTER_NEAREST: return linearFiltering ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
		case MIP_FILTER_LINEAR: return linearFiltering ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
		default: bug_branch("%d", (int)mipFiltering); return 0;
	}
}

static GLint makeMagFilter(bool linearFiltering)
{
	return linearFiltering ? GL_LINEAR : GL_NEAREST;
}

static GLint makeWrapMode(WrapMode mode)
{
	return mode == WRAP_CLAMP ? GL_CLAMP_TO_EDGE : GL_REPEAT;
}

static const PixelFormatDesc *swapRGBToPreferedOrder(const PixelFormatDesc *fmt)
{
	if(Gfx::preferBGR && fmt->id == PIXEL_RGB888)
		return &PixelFormatBGR888;
	else if(Gfx::preferBGRA && fmt->id == PIXEL_RGBA8888)
		return &PixelFormatBGRA8888;
	else
		return fmt;
}

static TextureConfig configWithLoadedImagePixmap(IG::PixmapDesc desc, bool makeMipmaps)
{
	TextureConfig config{desc};
	config.setWillGenerateMipmaps(makeMipmaps);
	return config;
}

static LockedTextureBuffer makeLockedTextureBuffer(IG::Pixmap pix, IG::WindowRect srcDirtyBounds, uint lockedLevel)
{
	LockedTextureBuffer lockBuff;
	lockBuff.set(pix, srcDirtyBounds, lockedLevel);
	return lockBuff;
}

static GLuint getTexturePBO()
{
	assert(texturePBO[texturePBOIdx]);
	auto pbo = texturePBO[texturePBOIdx];
	texturePBOIdx = (texturePBOIdx+1) % sizeofArray(texturePBO);
	return pbo;
}

void initTexturePBO()
{
	if(unlikely(usePBO && !texturePBO[0]))
	{
		glGenBuffers(sizeofArray(texturePBO), texturePBO);
	}
}

template<class T>
static CallResult initTextureCommon(T &texture, GfxImageSource &img, bool makeMipmaps)
{
	auto imgPix = img.lockPixmap();
	auto unlockImgPixmap = IG::scopeGuard([&](){ img.unlockPixmap(); });
	auto result = texture.init(configWithLoadedImagePixmap(imgPix, makeMipmaps));
	if(result != OK)
		return result;
	auto lockBuff = texture.lock(0);
	if(lockBuff)
	{
		if(imgPix)
		{
			//logDMsg("copying locked image pixmap to locked texture");
			imgPix.copy(0, 0, 0, 0, lockBuff.pixmap(), 0, 0);
		}
		else
		{
			//logDMsg("writing image to locked texture");
			img.write(lockBuff.pixmap());
		}
		texture.unlock(lockBuff);
	}
	else
	{
		if(imgPix)
		{
			//logDMsg("writing locked image pixmap to texture");
			texture.write(0, imgPix, {});
		}
		else
		{
			//logDMsg("writing image to texture");
			IG::ManagedPixmap texPix{imgPix.format};
			if(!texPix.init(imgPix.x, imgPix.y))
				return OUT_OF_MEMORY;
			img.write(texPix);
			texture.write(0, texPix, {});
		}
	}
	unlockImgPixmap();
	if(makeMipmaps)
		texture.generateMipmaps();
	return OK;
}

CallResult TextureSampler::init(TextureSamplerConfig config)
{
	deinit();
	magFilter = makeMagFilter(config.minLinearFilter());
	minFilter = makeMinFilter(config.magLinearFilter(), config.mipFilter());
	xWrapMode_ = makeWrapMode(config.xWrapMode());
	yWrapMode_ = makeWrapMode(config.yWrapMode());
	if(useSamplerObjects)
	{
		glGenSamplers(1, &name_);
		if(magFilter != GL_LINEAR) // GL_LINEAR is the default
			setSamplerParameteri(name_, GL_TEXTURE_MAG_FILTER, magFilter);
		if(minFilter != GL_NEAREST_MIPMAP_LINEAR) // GL_NEAREST_MIPMAP_LINEAR is the default
			setSamplerParameteri(name_, GL_TEXTURE_MIN_FILTER, minFilter);
		if(xWrapMode_ != GL_REPEAT) // GL_REPEAT is the default
			setSamplerParameteri(name_, GL_TEXTURE_WRAP_S, xWrapMode_);
		if(yWrapMode_ != GL_REPEAT) // GL_REPEAT​​ is the default
			setSamplerParameteri(name_, GL_TEXTURE_WRAP_T, yWrapMode_);
	}
	else
	{
		samplerNames++;
		name_ = samplerNames;
	}
	logMsg("created sampler:0x%X", name_);
	return OK;
}

void TextureSampler::deinit()
{
	if(useSamplerObjects && name_)
	{
		glDeleteSamplers(1, &name_);
	}
	*this = {};
}

void TextureSampler::bind()
{
	if(useSamplerObjects && currSampler.name() != name_)
	{
		//logMsg("bind sampler:0x%X", (int)name_);
		glBindSampler(0, name_);
	}
	currSampler = *this;
}

TextureSampler::operator bool() const
{
	return name_;
}

void TextureSampler::initDefaultClampSampler()
{
	if(!defaultClampSampler)
	{
		defaultClampSampler.init({});
	}
}

void TextureSampler::initDefaultNearestMipClampSampler()
{
	if(!defaultNearestMipClampSampler)
	{
		TextureSamplerConfig conf;
		conf.setMipFilter(MIP_FILTER_NEAREST);
		defaultNearestMipClampSampler.init(conf);
	}
}

void TextureSampler::initDefaultNoMipClampSampler()
{
	if(!defaultNoMipClampSampler)
	{
		TextureSamplerConfig conf;
		conf.setMipFilter(MIP_FILTER_NONE);
		defaultNoMipClampSampler.init(conf);
	}
}

void TextureSampler::initDefaultNoLinearNoMipClampSampler()
{
	if(!defaultNoLinearNoMipClampSampler)
	{
		TextureSamplerConfig conf;
		conf.setLinearFilter(false);
		conf.setMipFilter(MIP_FILTER_NONE);
		defaultNoLinearNoMipClampSampler.init(conf);
	}
}

void TextureSampler::initDefaultRepeatSampler()
{
	if(!defaultRepeatSampler)
	{
		TextureSamplerConfig conf;
		conf.setWrapMode(WRAP_REPEAT);
		defaultRepeatSampler.init(conf);
	}
}

void TextureSampler::initDefaultNearestMipRepeatSampler()
{
	if(!defaultNearestMipRepeatSampler)
	{
		TextureSamplerConfig conf;
		conf.setMipFilter(MIP_FILTER_NEAREST);
		conf.setWrapMode(WRAP_REPEAT);
		defaultNearestMipRepeatSampler.init(conf);
	}
}

void TextureSampler::bindDefaultClampSampler()
{
	assert(defaultClampSampler);
	defaultClampSampler.bind();
}

void TextureSampler::bindDefaultNearestMipClampSampler()
{
	assert(defaultNearestMipClampSampler);
	defaultNearestMipClampSampler.bind();
}

void TextureSampler::bindDefaultNoMipClampSampler()
{
	assert(defaultNoMipClampSampler);
	defaultNoMipClampSampler.bind();
}

void TextureSampler::bindDefaultNoLinearNoMipClampSampler()
{
	assert(defaultNoLinearNoMipClampSampler);
	defaultNoLinearNoMipClampSampler.bind();
}

void TextureSampler::bindDefaultRepeatSampler()
{
	assert(defaultRepeatSampler);
	defaultRepeatSampler.bind();
}

void TextureSampler::bindDefaultNearestMipRepeatSampler()
{
	assert(defaultNearestMipRepeatSampler);
	defaultNearestMipRepeatSampler.bind();
}

void GLTextureSampler::setTexParams(GLenum target)
{
	assert(!useSamplerObjects);
	setTexParameteri(target, GL_TEXTURE_MAG_FILTER, magFilter);
	setTexParameteri(target, GL_TEXTURE_MIN_FILTER, minFilter);
	setTexParameteri(target, GL_TEXTURE_WRAP_S, xWrapMode_);
	setTexParameteri(target, GL_TEXTURE_WRAP_T, yWrapMode_);
}

DirectTextureStorage::~DirectTextureStorage() {}

IG::Pixmap LockedTextureBuffer::pixmap()
{
	return pix;
}

IG::WindowRect LockedTextureBuffer::sourceDirtyRect()
{
	return srcDirtyRect;
}

LockedTextureBuffer::operator bool() const
{
	return (bool)pix;
}

void GLLockedTextureBuffer::set(IG::Pixmap pix, IG::WindowRect srcDirtyRect, uint lockedLevel)
{
	var_selfs(pix);
	var_selfs(srcDirtyRect);
	var_selfs(lockedLevel);
}

CallResult Texture::init(TextureConfig config)
{
	deinit();
	texName_ = newTex();
	if(config.willWriteOften())
	{
		if(usePBO)
		{
			glGenBuffers(1, &ownPBO);
			logMsg("made dedicated PBO:0x%X for texture", ownPBO);
		}
		#ifdef __ANDROID__
		else if(config.levels() == 1)
		{
			if(surfaceTextureConf.use)
			{
				auto *surfaceTex = new SurfaceTextureStorage;
				if(surfaceTex->init(texName_) == OK)
				{
					target = GL_TEXTURE_EXTERNAL_OES;
					directTex = surfaceTex;
				}
				else
				{
					logWarn("failed to create SurfaceTexture, falling back to normal texture");
					delete surfaceTex;
				}
			}
			else if(directTextureConf.useEGLImageKHR)
			{
				auto *gbTex = new GraphicBufferStorage;
				if(gbTex->init() == OK)
				{
					directTex = gbTex;
				}
				else
				{
					logWarn("failed to create EGL image, falling back to normal texture");
					delete gbTex;
				}
			}
		}
		#endif
	}
	if(config.willGenerateMipmaps() && !useImmutableTexStorage)
	{
		// when using glGenerateMipmaps exclusively, there is no need to define
		// all texture levels with glTexImage2D beforehand
		config.setLevels(1);
	}
	setFormat(config.pixmapDesc(), config.levels());
	return OK;
}

CallResult Texture::init(GfxImageSource &img, bool makeMipmaps)
{
	return initTextureCommon(*this, img, makeMipmaps);
}

void Texture::deinit()
{
	if(texName_)
	{
		logMsg("deinit texture:0x%X", texName_);
		delete directTex;
		deleteTex(texName_);
	}
	if(ownPBO)
	{
		logMsg("deleting PBO:0x%X", ownPBO);
		glcDeleteBuffers(1, &ownPBO);
	}
	*this = {};
}

uint Texture::bestAlignment(IG::Pixmap p)
{
	return unpackAlignForAddrAndPitch(p.data, p.pitch);
}

bool Texture::canUseMipmaps()
{
	return !directTex && textureSizeSupport.supportsMipmaps(pixDesc.x, pixDesc.y);
}

bool Texture::generateMipmaps()
{
	if(!canUseMipmaps())
		return false;
	logMsg("generating mipmaps for texture:0x%X", texName_);
	glcBindTexture(GL_TEXTURE_2D, texName_);
	Gfx::generateMipmaps(GL_TEXTURE_2D);
	if(!useImmutableTexStorage)
	{
		// all possible levels generated by glGenerateMipmap
		levels_ = fls(pixDesc.x | pixDesc.y);
	}
	return true;
}

CallResult Texture::setFormat(IG::PixmapDesc desc, uint levels)
{
	if(unlikely(!texName_))
		return INVALID_PARAMETER;
	if(directTex)
	{
		levels = 1;
		if(pixDesc.isSameGeometry(desc))
			logWarn("resizing with same dimensions %dx%d, should optimize caller code", desc.x, desc.y);
		auto result = directTex->setFormat(desc, texName_);
		if(result != OK)
			return result;
	}
	else
	{
		if(textureSizeSupport.supportsMipmaps(desc.x, desc.y))
		{
			if(!levels)
				levels = fls(desc.x | desc.y);
		}
		else
		{
			levels = 1;
		}
		if(useImmutableTexStorage)
		{
			if(levels_) // texture format was previously set
			{
				deleteTex(texName_);
				texName_ = newTex();
			}
			glcBindTexture(GL_TEXTURE_2D, texName_);
			auto internalFormat = makeGLSizedInternalFormat(desc.format);
			logMsg("texture:0x%X storage size:%dx%d levels:%d internal format:%s",
				texName_, desc.x, desc.y, levels, glImageFormatToString(internalFormat));
			handleGLErrors();
			glTexStorage2D(GL_TEXTURE_2D, levels, internalFormat, desc.x, desc.y);
			if(handleGLErrors([](GLenum, const char *err) { logErr("%s in glTexStorage2D", err); }))
			{
				return INVALID_PARAMETER;
			}
		}
		else
		{
			if(unlikely(levels_ && levels != levels_)) // texture format was previously set
			{
				deleteTex(texName_);
				texName_ = newTex();
			}
			glcBindTexture(GL_TEXTURE_2D, texName_);
			auto format = makeGLFormat(desc.format);
			auto dataType = makeGLDataType(desc.format);
			auto internalFormat = makeGLInternalFormat(desc.format);
			logMsg("texture:0x%X storage size:%dx%d levels:%d internal format:%s image format:%s:%s",
				texName_, desc.x, desc.y, levels, glImageFormatToString(internalFormat), glImageFormatToString(format), glDataTypeToString(dataType));
			handleGLErrors();
			uint w = desc.x, h = desc.y;
			iterateTimes(levels, i)
			{
				glTexImage2D(GL_TEXTURE_2D, i, internalFormat, w, h, 0, format, dataType, nullptr);
				if(handleGLErrors([](GLenum, const char *err) { logErr("%s in glTexImage2D", err); }))
				{
					return INVALID_PARAMETER;
				}
				w = std::max(1u, (w / 2));
				h = std::max(1u, (h / 2));
			}
		}
		if(ownPBO)
		{
			uint buffSize = desc.sizeOfPixels(desc.x * desc.y);
			glcBindBuffer(GL_PIXEL_UNPACK_BUFFER, ownPBO);
			glBufferData(GL_PIXEL_UNPACK_BUFFER, buffSize, nullptr, GL_STREAM_DRAW);
			logMsg("allocated PBO buffer bytes:%u", buffSize);
		}
	}
	assert(levels);
	levels_ = levels;
	pixDesc = desc;
	#ifdef CONFIG_GFX_OPENGL_SHADER_PIPELINE
	if(Config::Gfx::OPENGL_ES && target == GL_TEXTURE_EXTERNAL_OES)
		type_ = TEX_2D_EXTERNAL;
	else
		type_ = typeForPixelFormat(desc.format);
	#endif
	setSwizzleForFormat(desc.format, texName_, target);
	return OK;
}

void Texture::bind()
{
	if(!texName_)
	{
		bug_exit("tried to bind uninitialized texture");
		return;
	}
	setActiveTexture(texName_, target);
	if(!useSamplerObjects && currSampler.name() != sampler)
	{
		logMsg("setting sampler:0x%X for texture:0x%X", (int)currSampler.name(), texName_);
		sampler = currSampler.name();
		currSampler.setTexParams(target);
	}
}

void Texture::write(uint level, IG::Pixmap pixmap, IG::WP destPos, uint assumeAlign)
{
	//logDMsg("writing pixmap %dx%d to pos %dx%d", pixmap.x, pixmap.y, destPos.x, destPos.y);
	if(unlikely(!texName_))
	{
		logErr("can't write to uninitialized texture");
		return;
	}
	assert(destPos.x + pixmap.x <= (uint)size(level).x);
	assert(destPos.y + pixmap.y <= (uint)size(level).y);
	assert(pixmap.format.id == pixDesc.format.id);
	if(!assumeAlign)
		assumeAlign = unpackAlignForAddrAndPitch(pixmap.data, pixmap.pitch);
	if(directTex)
	{
		assert(level == 0);
		if(destPos != IG::WP{0, 0} || pixmap.x != (uint)size(0).x || pixmap.y != (uint)size(0).y)
		{
			bug_exit("partial write of direct texture unsupported, use lock()");
		}
		auto lockBuff = lock(0);
		if(!lockBuff)
		{
			return;
		}
		pixmap.copy(0, 0, 0, 0, lockBuff.pixmap(), 0, 0);
		unlock(lockBuff);
	}
	else
	{
		glcBindTexture(GL_TEXTURE_2D, texName_);
		if((ptrsize)pixmap.data % (ptrsize)assumeAlign != 0)
		{
			bug_exit("expected data from address %p to be aligned to %u bytes", pixmap.data, assumeAlign);
		}
		GLenum format = makeGLFormat(pixmap.format);
		GLenum dataType = makeGLDataType(pixmap.format);
		if(usePBO)
		{
			auto lockBuff = lock(level, {destPos.x, destPos.y, destPos.x + (int)pixmap.x, destPos.y + (int)pixmap.y});
			if(!lockBuff)
			{
				return;
			}
			pixmap.copy(0, 0, 0, 0, lockBuff.pixmap(), 0, 0);
			unlock(lockBuff);
		}
		else if(useUnpackRowLength || pixmap.pitchPixels() == pixmap.x)
		{
			glcPixelStorei(GL_UNPACK_ALIGNMENT, assumeAlign);
			if(useUnpackRowLength)
				glcPixelStorei(GL_UNPACK_ROW_LENGTH, pixmap.pitchPixels());
			handleGLErrors();
			glTexSubImage2D(GL_TEXTURE_2D, level, destPos.x, destPos.y,
				pixmap.x, pixmap.y, format, dataType, pixmap.data);
			if(handleGLErrors([](GLenum, const char *err) { logErr("%s in glTexSubImage2D", err); }))
			{
				return;
			}
		}
		else
		{
			// must copy to temp pixmap without extra pitch pixels
			static uint prevPixmapX = 0, prevPixmapY = 0;
			if(pixmap.x != prevPixmapX || pixmap.y != prevPixmapY) // don't spam log with repeated calls of same size pixmap
			{
				prevPixmapX = pixmap.x;
				prevPixmapY = pixmap.y;
				logDMsg("non-optimal texture write operation with %ux%u pixmap", pixmap.x, pixmap.y);
			}
			IG::Pixmap tempPix{pixmap};
			alignas(8) char tempPixData[pixmap.unpaddedSize()];
			tempPix.init(tempPixData, pixmap.x, pixmap.y);
			glcPixelStorei(GL_UNPACK_ALIGNMENT, unpackAlignForAddrAndPitch(nullptr, tempPix.pitch));
			pixmap.copy(0, 0, 0, 0, tempPix, 0, 0);
			handleGLErrors();
			glTexSubImage2D(GL_TEXTURE_2D, level, destPos.x, destPos.y,
				tempPix.x, tempPix.y, format, dataType, tempPix.data);
			if(handleGLErrors([](GLenum, const char *err) { logErr("%s in glTexSubImage2D", err); }))
			{
				return;
			}
		}
	}
}

void Texture::write(uint level, IG::Pixmap pixmap, IG::WP destPos)
{
	write(level, pixmap, destPos, 0);
}

void Texture::clear(uint level)
{
	IG::PixmapDesc levelPixDesc{pixDesc};
	levelPixDesc.x = size(level).x;
	levelPixDesc.y = size(level).y;
	void *tempPixData = mem_calloc(levelPixDesc.unpaddedSize());
	IG::Pixmap blankPix{levelPixDesc};
	blankPix.init(tempPixData, pixDesc.x, pixDesc.y);
	write(level, blankPix, {}, unpackAlignForAddrAndPitch(nullptr, blankPix.pitch));
	mem_free(tempPixData);
}

LockedTextureBuffer Texture::lock(uint level)
{
	if(directTex)
	{
		assert(level == 0);
		auto buff = directTex->lock(nullptr);
		IG::Pixmap pix{pixDesc};
		pix.data = (char*)buff.data;
		pix.pitch = buff.pitch;
		return makeLockedTextureBuffer(pix, {}, 0);
	}
	else if(usePBO)
	{
		return lock(level, {0, 0, size(level).x, size(level).y});
	}
	else
		return {}; // lock() not supported
}

LockedTextureBuffer Texture::lock(uint level, IG::WindowRect rect)
{
	assert(rect.x2  <= size(level).x);
	assert(rect.y2 <= size(level).y);
	if(directTex)
	{
		assert(level == 0);
		auto buff = directTex->lock(&rect);
		IG::Pixmap pix{pixDesc};
		pix.data = (char*)buff.data;
		pix.pitch = buff.pitch;
		return makeLockedTextureBuffer(pix, rect, 0);
	}
	else if(usePBO)
	{
		uint rangeBytes = pixDesc.sizeOfPixels(rect.xSize() * rect.ySize());
		void *data;
		if(ownPBO)
		{
			glcBindBuffer(GL_PIXEL_UNPACK_BUFFER, ownPBO);
			data = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, rangeBytes, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
			//logDMsg("mapped own PBO at addr:%p", data);
		}
		else
		{
			GLuint pbo = getTexturePBO();
			glcBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
			glBufferData(GL_PIXEL_UNPACK_BUFFER, rangeBytes, nullptr, GL_STREAM_DRAW);
			data = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, rangeBytes, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
			//logDMsg("mapped global PBO at addr:%p", data);
		}
		if(!data)
		{
			logErr("error mapping buffer");
			return {};
		}
		IG::Pixmap pix{pixDesc};
		pix.init(data, rect.xSize(), rect.ySize());
		return makeLockedTextureBuffer(pix, rect, level);
	}
	else
		return {}; // lock() not supported
}

void Texture::unlock(LockedTextureBuffer lockBuff)
{
	if(directTex)
		directTex->unlock(texName_);
	else if(usePBO)
	{
		auto pix = lockBuff.pixmap();
		IG::WP destPos = {lockBuff.sourceDirtyRect().x, lockBuff.sourceDirtyRect().y};
		//logDMsg("unmapped PBO");
		glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
		glcPixelStorei(GL_UNPACK_ALIGNMENT, unpackAlignForAddrAndPitch(nullptr, pix.pitch));
		glcPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
		GLenum format = makeGLFormat(pix.format);
		GLenum dataType = makeGLDataType(pix.format);
		handleGLErrors();
		glTexSubImage2D(GL_TEXTURE_2D, lockBuff.level(), destPos.x, destPos.y,
			pix.x, pix.y, format, dataType, nullptr);
		if(handleGLErrors([](GLenum, const char *err) { logErr("%s in glTexSubImage2D", err); }))
		{
			return;
		}
		// discard the data
		if(ownPBO)
		{
			glBufferData(GL_PIXEL_UNPACK_BUFFER, pixDesc.sizeOfPixels(pixDesc.x * pixDesc.y), nullptr, GL_STREAM_DRAW);
		}
		else
		{
			glBufferData(GL_PIXEL_UNPACK_BUFFER, 0, nullptr, GL_STREAM_DRAW);
		}
	}
}

IG::WP Texture::size(uint level) const
{
	uint w = pixDesc.x, h = pixDesc.y;
	iterateTimes(level, i)
	{
		w = std::max(1u, (w / 2));
		h = std::max(1u, (h / 2));
	}
	return {(int)w, (int)h};
}

IG::PixmapDesc Texture::pixmapDesc()
{
	return pixDesc;
}

bool Texture::compileDefaultProgram(uint mode)
{
	switch(mode)
	{
		bcase IMG_MODE_REPLACE:
			switch(type_)
			{
				case TEX_2D_1 : return texAlphaReplaceProgram.compile();
				case TEX_2D_2 : return texIntensityAlphaReplaceProgram.compile();
				case TEX_2D_4 : return texReplaceProgram.compile();
				case TEX_2D_EXTERNAL : return texExternalReplaceProgram.compile();
				default: bug_branch("%d", type_); return false;
			}
		bcase IMG_MODE_MODULATE:
			switch(type_)
			{
				case TEX_2D_1 : return texAlphaProgram.compile();
				case TEX_2D_2 : return texIntensityAlphaProgram.compile();
				case TEX_2D_4 : return texProgram.compile();
				case TEX_2D_EXTERNAL : return texExternalProgram.compile();
				default: bug_branch("%d", type_); return false;
			}
		bdefault: bug_branch("%d", type_); return false;
	}
}

void Texture::useDefaultProgram(uint mode, const Mat4 *modelMat)
{
	#ifndef CONFIG_GFX_OPENGL_SHADER_PIPELINE
	const uint type_ = TEX_2D_4;
	#endif
	switch(mode)
	{
		bcase IMG_MODE_REPLACE:
			switch(type_)
			{
				bcase TEX_2D_1 : texAlphaReplaceProgram.use(modelMat);
				bcase TEX_2D_2 : texIntensityAlphaReplaceProgram.use(modelMat);
				bcase TEX_2D_4 : texReplaceProgram.use(modelMat);
				bcase TEX_2D_EXTERNAL : texExternalReplaceProgram.use(modelMat);
			}
		bcase IMG_MODE_MODULATE:
			switch(type_)
			{
				bcase TEX_2D_1 : texAlphaProgram.use(modelMat);
				bcase TEX_2D_2 : texIntensityAlphaProgram.use(modelMat);
				bcase TEX_2D_4 : texProgram.use(modelMat);
				bcase TEX_2D_EXTERNAL : texExternalProgram.use(modelMat);
			}
	}
}

Texture::operator bool() const
{
	return texName();
}

CallResult PixmapTexture::init(TextureConfig config)
{
	if(config.willWriteOften() && config.levels() == 1)
	{
		// try without changing size when using direct pixel storage
		if(Texture::init(config) == OK)
			return OK;
	}
	auto origPixDesc = config.pixmapDesc();
	textureSizeSupport.findBufferXYPixels(config.pixmapDesc().x, config.pixmapDesc().y, origPixDesc.x, origPixDesc.y);
	auto result = Texture::init(config);
	if(result != OK)
		return result;
	if(!origPixDesc.isSameGeometry(config.pixmapDesc()))
		clear(0);
	updateUV({}, {(int)origPixDesc.x, (int)origPixDesc.y});
	return OK;
}

CallResult PixmapTexture::init(GfxImageSource &img, bool makeMipmaps)
{
	return initTextureCommon(*this, img, makeMipmaps);
}

CallResult PixmapTexture::setFormat(IG::PixmapDesc desc, uint levels)
{
	if(directTex)
	{
		return Texture::setFormat(desc, levels);
	}
	else
	{
		IG::PixmapDesc newPixDesc{desc};
		textureSizeSupport.findBufferXYPixels(newPixDesc.x, newPixDesc.y, desc.x, desc.y);
		auto result = Texture::setFormat(newPixDesc, levels);
		if(result != OK)
		{
			return result;
		}
		if(!desc.isSameGeometry(newPixDesc))
			clear(0);
		updateUV({}, {(int)desc.x, (int)desc.y});
		return OK;
	}
}

IG::Rect2<GTexC> PixmapTexture::uvBounds() const
{
	return uv;
}

void PixmapTexture::updateUV(IG::WP pixPos, IG::WP pixSize)
{
	uv.x = pixelToTexC((uint)pixPos.x, pixDesc.x);
	uv.y = pixelToTexC((uint)pixPos.y, pixDesc.y);
	uv.x2 = pixelToTexC((uint)(pixPos.x + pixSize.x), pixDesc.x);
	uv.y2 = pixelToTexC((uint)(pixPos.y + pixSize.y), pixDesc.y);
}

GLuint GLTexture::texName() const
{
	return texName_;
}

void GLTexture::setSwizzleForFormat(const PixelFormatDesc &format, GLuint tex, GLenum target)
{
	#if defined CONFIG_GFX_OPENGL_SHADER_PIPELINE
	if(useFixedFunctionPipeline)
		return;
	if(useTextureSwizzle && target == GL_TEXTURE_2D)
	{
		glcBindTexture(GL_TEXTURE_2D, tex);
		const GLint swizzleMaskRGBA[] {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA};
		const GLint swizzleMaskIA88[] {GL_RED, GL_RED, GL_RED, GL_GREEN};
		const GLint swizzleMaskA8[] {GL_ONE, GL_ONE, GL_ONE, GL_RED};
		#ifdef CONFIG_GFX_OPENGL_ES
		auto &swizzleMask = (format.id == PIXEL_IA88) ? swizzleMaskIA88
				: (format.id == PIXEL_A8) ? swizzleMaskA8
				: swizzleMaskRGBA;
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, swizzleMask[0]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, swizzleMask[1]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, swizzleMask[2]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, swizzleMask[3]);
		#else
		glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, (format.id == PIXEL_IA88) ? swizzleMaskIA88
				: (format.id == PIXEL_A8) ? swizzleMaskA8
				: swizzleMaskRGBA);
		#endif
	}
	#endif
}

}

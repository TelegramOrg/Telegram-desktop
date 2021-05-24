/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "calls/group/calls_group_viewport_opengl.h"

#include "calls/group/calls_group_viewport_tile.h"
#include "webrtc/webrtc_video_track.h"
#include "media/view/media_view_pip.h"

#include <QtGui/QOpenGLShader>

namespace Calls::Group {
namespace {

struct ShaderPart {
	QString header;
	QString body;
};

[[nodiscard]] QString VertexShader(const std::vector<ShaderPart> &parts) {
	const auto accumulate = [&](auto proj) {
		return ranges::accumulate(parts, QString(), std::plus<>(), proj);
	};
	return R"(
#version 120
attribute vec2 position;
)" + accumulate(&ShaderPart::header) + R"(
void main() {
	vec4 result = vec4(position, 0., 1.);
)" + accumulate(&ShaderPart::body) + R"(
	gl_Position = result;
}
)";
}

[[nodiscard]] QString FragmentShader(const std::vector<ShaderPart> &parts) {
	const auto accumulate = [&](auto proj) {
		return ranges::accumulate(parts, QString(), std::plus<>(), proj);
	};
	return R"(
#version 120
)" + accumulate(&ShaderPart::header) + R"(
void main() {
	vec4 result = vec4(0., 0., 0., 0.);
)" + accumulate(&ShaderPart::body) + R"(
	gl_FragColor = result;
}
)";
}

[[nodiscard]] ShaderPart VertexPassTextureCoord() {
	return {
		.header = R"(
attribute vec2 texcoord;
varying vec2 v_texcoord;
)",
		.body = R"(
	v_texcoord = texcoord;
)",
	};
}

[[nodiscard]] ShaderPart FragmentSampleARGB32Texture() {
	return {
		.header = R"(
varying vec2 v_texcoord;
uniform sampler2D s_texture;
)",
		.body = R"(
	result = texture2D(s_texture, v_texcoord);
	result = vec4(result.b, result.g, result.r, result.a);
)",
	};
}

[[nodiscard]] ShaderPart FragmentSampleYUV420Texture() {
	return {
		.header = R"(
varying vec2 v_texcoord;
uniform sampler2D y_texture;
uniform sampler2D u_texture;
uniform sampler2D v_texture;
)",
		.body = R"(
	float y = texture2D(y_texture, v_texcoord).r;
	float u = texture2D(u_texture, v_texcoord).r - 0.5;
	float v = texture2D(v_texture, v_texcoord).r - 0.5;
	result = vec4(y + 1.403 * v, y - 0.344 * u - 0.714 * v, y + 1.77 * u, 1);
)",
	};
}

[[nodiscard]] ShaderPart VertexViewportTransform() {
	return {
		.header = R"(
uniform vec2 viewport;
vec4 transform(vec4 position) {
	return vec4(
		vec2(-1, -1) + 2 * position.xy / viewport,
		position.z,
		position.w);
}
)",
		.body = R"(
	result = transform(result);
)",
	};
}

[[nodiscard]] ShaderPart FragmentRoundCorners() {
	return {
		.header = R"(
uniform vec4 roundRect;
uniform vec4 roundBg;
uniform float roundRadius;
float roundedCorner() {
	vec2 rectHalf = roundRect.zw / 2;
	vec2 rectCenter = roundRect.xy + rectHalf;
	vec2 fromRectCenter = abs(gl_FragCoord.xy - rectCenter);
	vec2 vectorRadius = vec2(roundRadius + 0.5, roundRadius + 0.5);
	vec2 fromCenterWithRadius = fromRectCenter + vectorRadius;
	vec2 fromRoundingCenter = max(fromCenterWithRadius, rectHalf)
		- rectHalf;
	float d = length(fromRoundingCenter) - roundRadius;
	return 1. - smoothstep(0., 1., d);
}
)",
		.body = R"(
	float rounded = roundedCorner();
	result = result * rounded + roundBg * (1. - rounded);
)",
	};
}

// Depends on FragmetSampleTexture().
[[nodiscard]] ShaderPart FragmentFrameColor() {
	return {
		.header = R"(
uniform vec4 frameBg;
)",
		.body = R"(
	vec2 textureHalf = vec2(0.5, 0.5);
	vec2 fromTextureCenter = abs(v_texcoord - textureHalf);
	vec2 fromTextureEdge = max(fromTextureCenter, textureHalf) - textureHalf;
	float outsideCheck = dot(fromTextureEdge, fromTextureEdge);
	float inside = step(outsideCheck, 0);
	result = result * inside + frameBg * (1. - inside);
)",
	};
}

[[nodiscard]] ShaderPart FragmentStaticColor() {
	return {
		.header = R"(
uniform vec4 s_color;
)",
		.body = R"(
	result = s_color;
)",
	};
}

not_null<QOpenGLShader*> MakeShader(
		not_null<QOpenGLShaderProgram*> program,
		QOpenGLShader::ShaderType type,
		const QString &source) {
	const auto result = new QOpenGLShader(type, program);
	if (!result->compileSourceCode(source)) {
		LOG(("Shader Compilation Failed: %1, error %2."
			).arg(source
			).arg(result->log()));
	}
	program->addShader(result);
	return result;
}

struct Program {
	not_null<QOpenGLShader*> vertex;
	not_null<QOpenGLShader*> fragment;
};

Program LinkProgram(
		not_null<QOpenGLShaderProgram*> program,
		std::variant<QString, not_null<QOpenGLShader*>> vertex,
		std::variant<QString, not_null<QOpenGLShader*>> fragment) {
	const auto vertexAsSource = v::is<QString>(vertex);
	const auto v = vertexAsSource
		? MakeShader(
			program,
			QOpenGLShader::Vertex,
			v::get<QString>(vertex))
		: v::get<not_null<QOpenGLShader*>>(vertex);
	if (!vertexAsSource) {
		program->addShader(v);
	}
	const auto fragmentAsSource = v::is<QString>(fragment);
	const auto f = fragmentAsSource
		? MakeShader(
			program,
			QOpenGLShader::Fragment,
			v::get<QString>(fragment))
		: v::get<not_null<QOpenGLShader*>>(fragment);
	if (!fragmentAsSource) {
		program->addShader(f);
	}
	if (!program->link()) {
		LOG(("Shader Link Failed: %1.").arg(program->log()));
	}
	return { v, f };
}

[[nodiscard]] QVector4D Uniform(const QRect &rect, GLfloat factor) {
	return QVector4D(
		rect.x() * factor,
		rect.y() * factor,
		rect.width() * factor,
		rect.height() * factor);
}

[[nodiscard]] QVector4D Uniform(const QColor &color) {
	return QVector4D(
		color.redF(),
		color.greenF(),
		color.blueF(),
		color.alphaF());
}

void FillRectVertices(GLfloat *coords, QRect rect, GLfloat factor) {
	coords[0] = coords[10] = rect.x() * factor;
	coords[1] = coords[11] = rect.y() * factor;
	coords[2] = (rect.x() + rect.width()) * factor;
	coords[3] = rect.y() * factor;
	coords[4] = coords[6] = (rect.x() + rect.width()) * factor;
	coords[5] = coords[7] = (rect.y() + rect.height()) * factor;
	coords[8] = rect.x() * factor;
	coords[9] = (rect.y() + rect.height()) * factor;
}

void FillTriangles(
		not_null<QOpenGLFunctions*> f,
		gsl::span<const GLfloat> coords,
		not_null<QOpenGLBuffer*> buffer,
		not_null<QOpenGLShaderProgram*> program,
		QSize viewportWithFactor,
		const QColor &color,
		Fn<void()> additional = nullptr) {
	Expects(coords.size() % 6 == 0);

	if (coords.empty()) {
		return;
	}
	buffer->bind();
	buffer->allocate(coords.data(), coords.size() * sizeof(GLfloat));

	f->glUseProgram(program->programId());
	program->setUniformValue("viewport", QSizeF(viewportWithFactor));
	program->setUniformValue("s_color", Uniform(color));

	GLint position = program->attributeLocation("position");
	f->glVertexAttribPointer(
		position,
		2,
		GL_FLOAT,
		GL_FALSE,
		2 * sizeof(GLfloat),
		nullptr);
	f->glEnableVertexAttribArray(position);

	if (additional) {
		additional();
	}

	f->glDrawArrays(GL_TRIANGLES, 0, coords.size() / 2);

	f->glDisableVertexAttribArray(position);
}

} // namespace

Viewport::RendererGL::RendererGL(not_null<Viewport*> owner)
: _owner(owner) {
}

void Viewport::RendererGL::free(const Textures &textures) {
	_texturesToFree.push_back(textures);
}

void Viewport::RendererGL::init(
		not_null<QOpenGLWidget*> widget,
		not_null<QOpenGLFunctions*> f) {
	_factor = widget->devicePixelRatio();
	_frameBuffer.emplace();
	_frameBuffer->setUsagePattern(QOpenGLBuffer::DynamicDraw);
	_frameBuffer->create();
	_yuv420Program.emplace();
	_frameVertexShader = LinkProgram(
		&*_yuv420Program,
		VertexShader({
			VertexViewportTransform(),
			VertexPassTextureCoord(),
		}),
		FragmentShader({
			FragmentSampleYUV420Texture(),
			FragmentFrameColor(),
			FragmentRoundCorners(),
		})).vertex;


	_bgBuffer.emplace();
	_bgBuffer->setUsagePattern(QOpenGLBuffer::DynamicDraw);
	_bgBuffer->create();
	_bgProgram.emplace();
	LinkProgram(
		&*_bgProgram,
		VertexShader({ VertexViewportTransform() }),
		FragmentShader({ FragmentStaticColor() }));
}

void Viewport::RendererGL::ensureARGB32Program() {
	Expects(_frameVertexShader != nullptr);

	_argb32Program.emplace();
	LinkProgram(
		&*_argb32Program,
		_frameVertexShader,
		FragmentShader({
			FragmentSampleARGB32Texture(),
			FragmentFrameColor(),
			FragmentRoundCorners(),
		}));
}

void Viewport::RendererGL::deinit(
		not_null<QOpenGLWidget*> widget,
		not_null<QOpenGLFunctions*> f) {
	_frameBuffer = std::nullopt;
	_bgBuffer = std::nullopt;
	_frameVertexShader = nullptr;
	_argb32Program = std::nullopt;
	_yuv420Program = std::nullopt;
	_bgProgram = std::nullopt;
	for (const auto &tile : _owner->_tiles) {
		if (const auto textures = tile->takeTextures()) {
			free(textures);
		}
	}
	freeTextures(f);
}

void Viewport::RendererGL::resize(
		not_null<QOpenGLWidget*> widget,
		not_null<QOpenGLFunctions*> f,
		int w,
		int h) {
	_factor = widget->devicePixelRatio();
	_viewport = QSize(w, h);
	f->glViewport(0, 0, w * _factor, h * _factor);
}

void Viewport::RendererGL::paint(
		not_null<QOpenGLWidget*> widget,
		not_null<QOpenGLFunctions*> f) {
	_factor = widget->devicePixelRatio();
	fillBackground(f);
	for (const auto &tile : _owner->_tiles) {
		paintTile(f, tile.get());
	}
	freeTextures(f);
}

void Viewport::RendererGL::fillBackground(not_null<QOpenGLFunctions*> f) {
	const auto radius = st::roundRadiusLarge;
	const auto radiuses = QMargins{ radius, radius, radius, radius };
	auto bg = QRegion(QRect(QPoint(), _viewport));
	for (const auto &tile : _owner->_tiles) {
		bg -= tileGeometry(tile.get()).marginsRemoved(radiuses);
	}
	if (bg.isEmpty()) {
		return;
	}
	_bgTriangles.resize((bg.end() - bg.begin()) * 12);
	auto coords = _bgTriangles.data();
	for (const auto rect : bg) {
		FillRectVertices(coords, rect, _factor);
		coords += 12;
	}
	FillTriangles(
		f,
		_bgTriangles,
		&*_bgBuffer,
		&*_bgProgram,
		_viewport * _factor,
		st::groupCallBg->c);
}

void Viewport::RendererGL::paintTile(
		not_null<QOpenGLFunctions*> f,
		not_null<VideoTile*> tile) {
	const auto track = tile->track();
	const auto data = track->frameWithInfo(false);
	if (data.format == Webrtc::FrameFormat::None) {
		return;
	}

	const auto geometry = tileGeometry(tile);
	const auto x = geometry.x();
	const auto y = geometry.y();
	const auto width = geometry.width();
	const auto height = geometry.height();
	const auto expand = !_owner->wide()/* && !tile->screencast()*/;
	const auto scaled = Media::View::FlipSizeByRotation(
		data.yuv420->size,
		data.rotation
	).scaled(
		QSize(width, height),
		(expand ? Qt::KeepAspectRatioByExpanding : Qt::KeepAspectRatio));
	if (scaled.isEmpty()) {
		return;
	}
	const auto left = (width - scaled.width()) / 2;
	const auto top = (height - scaled.height()) / 2;
	const auto right = left + scaled.width();
	const auto bottom = top + scaled.height();
	const auto radius = GLfloat(st::roundRadiusLarge);
	auto dleft = float(left) / scaled.width();
	auto dright = float(width - left) / scaled.width();
	auto dtop = float(top) / scaled.height();
	auto dbottom = float(height - top) / scaled.height();
	const auto swap = (((data.rotation / 90) % 2) == 1);
	if (swap) {
		std::swap(dleft, dtop);
		std::swap(dright, dbottom);
	}
	auto texCoord = std::array<std::array<GLfloat, 2>, 4> { {
		{ { -dleft, 1.f + dtop } },
		{ { dright, 1.f + dtop } },
		{ { dright, 1.f - dbottom } },
		{ { -dleft, 1.f - dbottom } },
	} };
	if (data.rotation > 0) {
		std::rotate(
			texCoord.begin(),
			texCoord.begin() + (data.rotation / 90),
			texCoord.end());
	}
	const GLfloat coords[] = {
		x * _factor, y * _factor, texCoord[0][0], texCoord[0][1],
		(x + width) * _factor, y * _factor, texCoord[1][0], texCoord[1][1],
		(x + width) * _factor, (y + height) * _factor, texCoord[2][0], texCoord[2][1],
		x * _factor, (y + height) * _factor, texCoord[3][0], texCoord[3][1],
	};

	tile->ensureTexturesCreated(f);
	const auto &textures = tile->textures();
	const auto upload = (textures.trackIndex != data.index);
	const auto uploadOne = [&](GLint internalformat, GLint format, QSize size, int stride, const void *data) {
		f->glPixelStorei(GL_UNPACK_ROW_LENGTH, stride);
		f->glTexImage2D(
			GL_TEXTURE_2D,
			0,
			internalformat,
			size.width(),
			size.height(),
			0,
			format,
			GL_UNSIGNED_BYTE,
			data);
		f->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	};
	if (upload) {
		textures.textureIndex = 1 - textures.textureIndex;
	}
	const auto rgba = (data.format == Webrtc::FrameFormat::ARGB32);
	if (rgba) {
		ensureARGB32Program();
		const auto texture = textures.values[textures.textureIndex];
		f->glUseProgram(_argb32Program->programId());
		f->glActiveTexture(GL_TEXTURE0);
		f->glBindTexture(GL_TEXTURE_2D, texture);
		if (upload) {
			const auto &image = data.original;
			const auto stride = image.bytesPerLine() / 4;
			const auto data = image.constBits();
			uploadOne(GL_RGB, GL_RGBA, image.size(), stride, data);
		}
		_argb32Program->setUniformValue("s_texture", GLint(0));
	} else {
		const auto yuv = data.yuv420;
		const auto otherSize = yuv->chromaSize;
		const auto textureY = textures.values[textures.textureIndex * 3 + 0];
		const auto textureU = textures.values[textures.textureIndex * 3 + 1];
		const auto textureV = textures.values[textures.textureIndex * 3 + 2];
		f->glUseProgram(_yuv420Program->programId());
		f->glActiveTexture(GL_TEXTURE0);
		f->glBindTexture(GL_TEXTURE_2D, textureY);
		if (upload) {
			uploadOne(GL_RED, GL_RED, yuv->size, yuv->y.stride, yuv->y.data);
		}
		f->glActiveTexture(GL_TEXTURE1);
		f->glBindTexture(GL_TEXTURE_2D, textureU);
		if (upload) {
			uploadOne(GL_RED, GL_RED, otherSize, yuv->u.stride, yuv->u.data);
		}
		f->glActiveTexture(GL_TEXTURE2);
		f->glBindTexture(GL_TEXTURE_2D, textureV);
		if (upload) {
			uploadOne(GL_RED, GL_RED, otherSize, yuv->v.stride, yuv->v.data);
		}
		_yuv420Program->setUniformValue("y_texture", GLint(0));
		_yuv420Program->setUniformValue("u_texture", GLint(1));
		_yuv420Program->setUniformValue("v_texture", GLint(2));
	}
	tile->track()->markFrameShown();

	_frameBuffer->bind();
	_frameBuffer->allocate(coords, sizeof(coords));

	const auto program = rgba ? &*_argb32Program : &*_yuv420Program;

	program->setUniformValue("viewport", QSizeF(_viewport * _factor));
	program->setUniformValue(
		"frameBg",
		Uniform(st::groupCallMembersBg->c));
	program->setUniformValue("roundRadius", radius * _factor);
	program->setUniformValue("roundRect", Uniform(geometry, _factor));
	program->setUniformValue("roundBg", Uniform(st::groupCallBg->c));

	GLint position = program->attributeLocation("position");
	f->glVertexAttribPointer(
		position,
		2,
		GL_FLOAT,
		GL_FALSE,
		4 * sizeof(GLfloat),
		nullptr);
	f->glEnableVertexAttribArray(position);

	GLint texcoord = program->attributeLocation("texcoord");
	f->glVertexAttribPointer(
		texcoord,
		2,
		GL_FLOAT,
		GL_FALSE,
		4 * sizeof(GLfloat),
		reinterpret_cast<const void*>(2 * sizeof(GLfloat)));
	f->glEnableVertexAttribArray(texcoord);

	f->glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

	f->glDisableVertexAttribArray(position);
}

QRect Viewport::RendererGL::tileGeometry(not_null<VideoTile*> tile) const {
	const auto raster = tile->geometry();
	return {
		raster.x(),
		_viewport.height() - raster.y() - raster.height(),
		raster.width(),
		raster.height(),
	};
}

void Viewport::RendererGL::freeTextures(not_null<QOpenGLFunctions*> f) {
	for (const auto &textures : base::take(_texturesToFree)) {
		f->glDeleteTextures(textures.values.size(), textures.values.data());
	}
}

} // namespace Calls::Group
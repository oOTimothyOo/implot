#ifdef IMPLOT_ENABLE_OPENGL3_ACCELERATION

#include "../implot.h"
#include "../implot_internal.h"

#if defined(IMGUI_IMPL_OPENGL_ES2)
#include <GLES2/gl2.h>
// About Desktop OpenGL function loaders:
//  Modern desktop OpenGL doesn't have a standard portable header file to load OpenGL function pointers.
//  Helper libraries are often used for this purpose! Here we are supporting a few common ones (gl3w, glew, glad).
//  You may use another loader/header of your choice (glext, glLoadGen, etc.), or chose to manually implement your own.
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GL3W)
#include <GL/gl3w.h>            // Initialize with gl3wInit()
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLEW)
#include <GL/glew.h>            // Initialize with glewInit()
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLAD)
#include <glad/glad.h>          // Initialize with gladLoadGL()
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLAD2)
#include <glad/gl.h>            // Initialize with gladLoadGL(...) or gladLoaderLoadGL()
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLBINDING2)
#define GLFW_INCLUDE_NONE       // GLFW including OpenGL headers causes ambiguity or multiple definition errors.
#include <glbinding/Binding.h>  // Initialize with glbinding::Binding::initialize()
#include <glbinding/gl/gl.h>
using namespace gl;
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLBINDING3)
#define GLFW_INCLUDE_NONE       // GLFW including OpenGL headers causes ambiguity or multiple definition errors.
#include <glbinding/glbinding.h>// Initialize with glbinding::initialize()
#include <glbinding/gl/gl.h>
using namespace gl;
#else
#include IMGUI_IMPL_OPENGL_LOADER_CUSTOM
#endif

namespace ImPlot {
namespace Backends {

struct HeatmapData
{
	GLuint HeatmapTexID;
	GLuint ColormapTexID;
	float MinValue;
	float MaxValue;
};

struct OpenGLContextData
{
	GLuint g_ShaderProgram = 0;                 ///< Shader ID for the heatmap shader
	GLuint g_AttribLocationHeatmapSampler = 0;  ///< Attribute location for the heatmap texture
	GLuint g_AttribLocationColormapSampler = 0; ///< Attribute location for the colormap texture
	GLuint g_AttribLocationProjection = 0;      ///< Attribute location for the projection matrix uniform
	GLuint g_AttribLocationMinValue = 0;        ///< Attribute location for the minimum value uniform
	GLuint g_AttribLocationMaxValue = 0;        ///< Attribute location for the maximum value uniform

	GLuint g_AttribLocationImGuiProjection = 0; ///< Attribute location for the projection matrix uniform (ImGui default shader)

	ImVector<HeatmapData> HeatmapDataList;      ///< Array of heatmap data
	ImVector<GLuint> ColormapIDs;               ///< Texture IDs of the colormap textures
	ImGuiStorage PlotIDs;                       ///< PlotID <-> Heatmap array index table
};

static OpenGLContextData Context;

static void CreateShader(const ImDrawList*, const ImDrawCmd*)
{
	GLuint CurrentShader;
	glGetIntegerv(GL_CURRENT_PROGRAM, (GLint*)&CurrentShader);

	Context.g_AttribLocationImGuiProjection = glGetUniformLocation(CurrentShader, "ProjMtx");

	GLuint g_AttribLocationVtxPos   = (GLuint)glGetAttribLocation(CurrentShader, "Position");
	GLuint g_AttribLocationVtxUV    = (GLuint)glGetAttribLocation(CurrentShader, "UV");
	GLuint g_AttribLocationVtxColor = (GLuint)glGetAttribLocation(CurrentShader, "Color");

	const GLchar* VertexShaderCode_t =
		"#version 330 core\n"
		"precision mediump float;\n"
		"layout (location = %d) in vec2 Position;\n"
		"layout (location = %d) in vec2 UV;\n"
		"layout (location = %d) in vec4 Color;\n"
		"\n"
		"uniform mat4 ProjMtx;\n"
		"out vec2 Frag_UV;\n"
		"\n"
		"void main()\n"
		"{\n"
		"	Frag_UV = UV;\n"
		"	gl_Position = ProjMtx * vec4(Position.xy, 0.0f, 1.0f);\n"
		"}\n";

	const GLchar* FragmentShaderCode =
		"#version 330 core\n"
		"precision mediump float;\n"
		"\n"
		"in vec2 Frag_UV;\n"
		"out vec4 Out_Color;\n"
		"\n"
		"uniform sampler1D colormap;\n"
		"uniform sampler2D heatmap;\n"
		"uniform float min_val;\n"
		"uniform float max_val;\n"
		"\n"
		"void main()\n"
		"{\n"
		"	float offset = (texture(heatmap, Frag_UV).r - min_val) / (max_val - min_val);\n"
		"	Out_Color = texture(colormap, clamp(offset, 0.0f, 1.0f));\n"
		"}\n";

	GLchar* VertexShaderCode = new GLchar[512];
	snprintf(VertexShaderCode, 512, VertexShaderCode_t, g_AttribLocationVtxPos, g_AttribLocationVtxUV, g_AttribLocationVtxColor);

	GLuint g_VertexShader = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(g_VertexShader, 1, &VertexShaderCode, nullptr);
	glCompileShader(g_VertexShader);

	GLuint g_FragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(g_FragmentShader, 1, &FragmentShaderCode, nullptr);
	glCompileShader(g_FragmentShader);

	Context.g_ShaderProgram = glCreateProgram();
	glAttachShader(Context.g_ShaderProgram, g_VertexShader);
	glAttachShader(Context.g_ShaderProgram, g_FragmentShader);
	glLinkProgram(Context.g_ShaderProgram);

	glDetachShader(Context.g_ShaderProgram, g_VertexShader);
	glDetachShader(Context.g_ShaderProgram, g_FragmentShader);
	glDeleteShader(g_VertexShader);
	glDeleteShader(g_FragmentShader);

	Context.g_AttribLocationHeatmapSampler  = glGetUniformLocation(Context.g_ShaderProgram, "heatmap");
	Context.g_AttribLocationColormapSampler = glGetUniformLocation(Context.g_ShaderProgram, "colormap");
	Context.g_AttribLocationProjection      = glGetUniformLocation(Context.g_ShaderProgram, "ProjMtx");
	Context.g_AttribLocationMinValue        = glGetUniformLocation(Context.g_ShaderProgram, "min_val");
	Context.g_AttribLocationMaxValue        = glGetUniformLocation(Context.g_ShaderProgram, "max_val");

	glUseProgram(Context.g_ShaderProgram);
	glUniform1i(Context.g_AttribLocationHeatmapSampler, 0); // Set texture slot of heatmap texture
	glUniform1i(Context.g_AttribLocationColormapSampler, 1); // Set texture slot of colormap texture

	delete[] VertexShaderCode;
}

static void RenderCallback(const ImDrawList*, const ImDrawCmd* cmd)
{
	int plotID = (int)(intptr_t)cmd->UserCallbackData;
	int plotIdx = Context.PlotIDs.GetInt(plotID, -1);
	HeatmapData& data = Context.HeatmapDataList[plotIdx];

	GLuint CurrentShader;
	glGetIntegerv(GL_CURRENT_PROGRAM, (GLint*)&CurrentShader);

	// Get projection matrix of current shader
	float OrthoProjection[4][4];
	glGetUniformfv(CurrentShader, Context.g_AttribLocationImGuiProjection, &OrthoProjection[0][0]);

	// Enable our shader
	glUseProgram(Context.g_ShaderProgram);

	glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, data.HeatmapTexID); // Set texture ID of data
	glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_1D, data.ColormapTexID); // Set texture ID of colormap

	glUniformMatrix4fv(Context.g_AttribLocationProjection, 1, GL_FALSE, &OrthoProjection[0][0]);
	glUniform1f(Context.g_AttribLocationMinValue, data.MinValue); // Set minimum range
	glUniform1f(Context.g_AttribLocationMaxValue, data.MaxValue); // Set maximum range
}

static void UnbindTexture(const ImDrawList*, const ImDrawCmd*)
{
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, 0);
}

static void SetTextureData(int plotID, const void* data, GLsizei rows, GLsizei cols, GLenum type)
{
	int idx = Context.PlotIDs.GetInt(plotID, -1);
	GLuint texID = Context.HeatmapDataList[idx].HeatmapTexID;

	// Set heatmap data
	glBindTexture(GL_TEXTURE_2D, texID);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, cols, rows, 0, GL_RED, type, data);
}

void OpenGL3_AddColormap(const ImU32* keys, int count, bool qual)
{
	GLuint texID;
	glGenTextures(1, &texID);
	glBindTexture(GL_TEXTURE_1D, texID);
	glTexImage1D(GL_TEXTURE_1D, 0, GL_RGBA, count, 0, GL_RGBA, GL_UNSIGNED_BYTE, keys);
	glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, qual ? GL_NEAREST : GL_LINEAR);
	glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, qual ? GL_NEAREST : GL_LINEAR);
	glBindTexture(GL_TEXTURE_1D, 0);

	Context.ColormapIDs.push_back(texID);
}

static GLuint CreateHeatmapTexture()
{
	GLuint textureID;
	glGenTextures(1, &textureID);
	glBindTexture(GL_TEXTURE_2D, textureID);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glBindTexture(GL_TEXTURE_2D, 0);

	return textureID;
}

void OpenGL3_SetHeatmapData(int plotID, const ImS8* values, int rows, int cols) { SetTextureData(plotID, values, rows, cols, GL_BYTE); }
void OpenGL3_SetHeatmapData(int plotID, const ImU8* values, int rows, int cols) { SetTextureData(plotID, values, rows, cols, GL_UNSIGNED_BYTE); }
void OpenGL3_SetHeatmapData(int plotID, const ImS16* values, int rows, int cols) { SetTextureData(plotID, values, rows, cols, GL_SHORT); }
void OpenGL3_SetHeatmapData(int plotID, const ImU16* values, int rows, int cols) { SetTextureData(plotID, values, rows, cols, GL_UNSIGNED_SHORT); }
void OpenGL3_SetHeatmapData(int plotID, const ImS32* values, int rows, int cols) { SetTextureData(plotID, values, rows, cols, GL_INT); }
void OpenGL3_SetHeatmapData(int plotID, const ImU32* values, int rows, int cols) { SetTextureData(plotID, values, rows, cols, GL_UNSIGNED_INT); }
void OpenGL3_SetHeatmapData(int plotID, const float* values, int rows, int cols) { SetTextureData(plotID, values, rows, cols, GL_FLOAT); }

void OpenGL3_SetHeatmapData(int plotID, const double* values, int rows, int cols)
{
	float* values2 = new float[rows*cols];

	for(int i = 0; i < rows*cols; i++)
		values2[i] = (float)values[i];

	SetTextureData(plotID, values2, rows, cols, GL_FLOAT);

	delete[] values2;
}

void OpenGL3_SetHeatmapData(int plotID, const ImS64* values, int rows, int cols)
{
	ImS32* values2 = new ImS32[rows*cols];

	for(int i = 0; i < rows*cols; i++)
		values2[i] = (ImS32)values[i];

	SetTextureData(plotID, values2, rows, cols, GL_INT);

	delete[] values2;
}

void OpenGL3_SetHeatmapData(int plotID, const ImU64* values, int rows, int cols)
{
	ImU32* values2 = new ImU32[rows*cols];

	for(int i = 0; i < rows*cols; i++)
		values2[i] = (ImU32)values[i];

	SetTextureData(plotID, values2, rows, cols, GL_UNSIGNED_INT);

	delete[] values2;
}

void OpenGL3_RenderHeatmap(int plotID, ImDrawList& DrawList, const ImVec2& bounds_min, const ImVec2& bounds_max, float scale_min, float scale_max, ImPlotColormap colormap)
{
	int idx = Context.PlotIDs.GetInt(plotID, -1);

	if(idx < 0)
	{
		// New entry
		HeatmapData data;
		data.HeatmapTexID = CreateHeatmapTexture();
		data.ColormapTexID = Context.ColormapIDs[colormap];
		data.MinValue = scale_min;
		data.MaxValue = scale_max;

		Context.PlotIDs.SetInt(plotID, Context.HeatmapDataList.Size);
		Context.HeatmapDataList.push_back(data);
	}
	else
	{
		HeatmapData& data = Context.HeatmapDataList[idx];
		data.ColormapTexID = Context.ColormapIDs[colormap];
		data.MinValue = scale_min;
		data.MaxValue = scale_max;
	}

	if(Context.g_ShaderProgram == 0)
		DrawList.AddCallback(CreateShader, nullptr);

	DrawList.AddCallback(RenderCallback, (void*)(intptr_t)plotID);
	DrawList.PrimReserve(6, 4);
	DrawList.PrimRectUV(bounds_min, bounds_max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), 0);
	DrawList.AddCallback(UnbindTexture, nullptr);
	DrawList.AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

void OpenGL3_BustPlotCache()
{
	for(const HeatmapData& data : Context.HeatmapDataList)
		glDeleteTextures(1, &data.HeatmapTexID);

	Context.HeatmapDataList.clear();
	Context.PlotIDs.Clear();
}

}
}

#endif

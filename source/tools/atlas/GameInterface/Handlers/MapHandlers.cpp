/* Copyright (C) 2016 Wildfire Games.
 * This file is part of 0 A.D.
 *
 * 0 A.D. is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * 0 A.D. is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with 0 A.D.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "precompiled.h"

#include "MessageHandler.h"
#include "../GameLoop.h"
#include "../CommandProc.h"

#include "graphics/GameView.h"
#include "graphics/LOSTexture.h"
#include "graphics/MapWriter.h"
#include "graphics/Patch.h"
#include "graphics/Terrain.h"
#include "graphics/TerrainTextureEntry.h"
#include "graphics/TerrainTextureManager.h"
#include "lib/bits.h"
#include "lib/file/file.h"
#include "lib/tex/tex.h"
#include "maths/MathUtil.h"
#include "ps/CLogger.h"
#include "ps/Filesystem.h"
#include "ps/Game.h"
#include "ps/GameSetup/GameSetup.h"
#include "ps/Loader.h"
#include "ps/World.h"
#include "renderer/Renderer.h"
#include "renderer/WaterManager.h"
#include "scriptinterface/ScriptInterface.h"
#include "simulation2/Simulation2.h"
#include "simulation2/components/ICmpPlayer.h"
#include "simulation2/components/ICmpPlayerManager.h"
#include "simulation2/components/ICmpPosition.h"
#include "simulation2/components/ICmpRangeManager.h"
#include "simulation2/components/ICmpTerrain.h"
#include "simulation2/system/ParamNode.h"

namespace
{
	void InitGame()
	{
		if (g_Game)
		{
			delete g_Game;
			g_Game = NULL;
		}

		g_Game = new CGame(false, false);

		// Default to player 1 for playtesting
		g_Game->SetPlayerID(1);
	}

	void StartGame(JS::MutableHandleValue attrs)
	{
		g_Game->StartGame(attrs, "");

		// TODO: Non progressive load can fail - need a decent way to handle this
		LDR_NonprogressiveLoad();

		// Disable fog-of-war - this must be done before starting the game,
		// as visual actors cache their visibility state on first render.
		CmpPtr<ICmpRangeManager> cmpRangeManager(*g_Game->GetSimulation2(), SYSTEM_ENTITY);
		if (cmpRangeManager)
			cmpRangeManager->SetLosRevealAll(-1, true);

		PSRETURN ret = g_Game->ReallyStartGame();
		ENSURE(ret == PSRETURN_OK);
	}
}

namespace AtlasMessage {

QUERYHANDLER(GenerateMap)
{
	try
	{
		InitGame();

		// Random map
		ScriptInterface& scriptInterface = g_Game->GetSimulation2()->GetScriptInterface();
		JSContext* cx = scriptInterface.GetContext();
		JSAutoRequest rq(cx);

		JS::RootedValue settings(cx);
		scriptInterface.ParseJSON(*msg->settings, &settings);
		scriptInterface.SetProperty(settings, "mapType", std::string("random"));

		JS::RootedValue attrs(cx);
		scriptInterface.Eval("({})", &attrs);
		scriptInterface.SetProperty(attrs, "mapType", std::string("random"));
		scriptInterface.SetProperty(attrs, "script", std::wstring(*msg->filename));
		scriptInterface.SetProperty(attrs, "settings", settings);

		StartGame(&attrs);

		msg->status = 0;
	}
	catch (PSERROR_Game_World_MapLoadFailed&)
	{
		// Cancel loading
		LDR_Cancel();

		// Since map generation failed and we don't know why, use the blank map as a fallback

		InitGame();

		ScriptInterface& scriptInterface = g_Game->GetSimulation2()->GetScriptInterface();
		JSContext* cx = scriptInterface.GetContext();
		JSAutoRequest rq(cx);

		JS::RootedValue settings(cx);
		scriptInterface.Eval("({})", &settings);
		// Set up 8-element array of empty objects to satisfy init
		JS::RootedValue playerData(cx);
		scriptInterface.Eval("([])", &playerData);
		for (int i = 0; i < 8; ++i)
		{
			JS::RootedValue player(cx);
			scriptInterface.Eval("({})", &player);
			scriptInterface.SetPropertyInt(playerData, i, player);
		}
		scriptInterface.SetProperty(settings, "mapType", std::string("scenario"));
		scriptInterface.SetProperty(settings, "PlayerData", playerData);

		JS::RootedValue atts(cx);
		scriptInterface.Eval("({})", &atts);
		scriptInterface.SetProperty(atts, "mapType", std::string("scenario"));
		scriptInterface.SetProperty(atts, "map", std::wstring(L"maps/scenarios/_default"));
		scriptInterface.SetProperty(atts, "settings", settings);
		StartGame(&atts);

		msg->status = -1;
	}
}

MESSAGEHANDLER(LoadMap)
{
	InitGame();

	ScriptInterface& scriptInterface = g_Game->GetSimulation2()->GetScriptInterface();
	JSContext* cx = scriptInterface.GetContext();
	JSAutoRequest rq(cx);

	// Scenario
	CStrW map = *msg->filename;
	CStrW mapBase = map.BeforeLast(L".pmp"); // strip the file extension, if any

	JS::RootedValue attrs(cx);
	scriptInterface.Eval("({})", &attrs);
	scriptInterface.SetProperty(attrs, "mapType", std::string("scenario"));
	scriptInterface.SetProperty(attrs, "map", std::wstring(mapBase));

	StartGame(&attrs);
}

MESSAGEHANDLER(ImportHeightmap)
{
	CStrW src = *msg->filename;

	size_t fileSize;
	shared_ptr<u8> fileData;

	// read in image file
	File file;
	if (file.Open(src, O_RDONLY) < 0)
	{
		LOGERROR("Failed to load heightmap.");
		return;
	}

	fileSize = lseek(file.Descriptor(), 0, SEEK_END);
	lseek(file.Descriptor(), 0, SEEK_SET);

	fileData = shared_ptr<u8>(new u8[fileSize]);

	if (read(file.Descriptor(), fileData.get(), fileSize) < 0)
	{
		LOGERROR("Failed to read heightmap image.");
		file.Close();
		return;
	}

	file.Close();

	// decode to a raw pixel format
	Tex tex;
	if (tex.decode(fileData, fileSize) < 0)
	{
		LOGERROR("Failed to decode heightmap.");
		return;
	}

	// Convert to uncompressed BGRA with no mipmaps
	if (tex.transform_to((tex.m_Flags | TEX_BGR | TEX_ALPHA) & ~(TEX_DXT | TEX_MIPMAPS)) < 0)
	{
		LOGERROR("Failed to transform heightmap.");
		return;
	}

	// pick smallest side of texture; truncate if not divisible by PATCH_SIZE
	ssize_t terrainSize = std::min(tex.m_Width, tex.m_Height);
	terrainSize -= terrainSize % PATCH_SIZE;

	// resize terrain to heightmap size
	CTerrain* terrain = g_Game->GetWorld()->GetTerrain();
	terrain->Resize(terrainSize / PATCH_SIZE);

	// copy heightmap data into map
	u16* heightmap = g_Game->GetWorld()->GetTerrain()->GetHeightMap();
	ssize_t hmSize = g_Game->GetWorld()->GetTerrain()->GetVerticesPerSide();

	u8* mapdata = tex.get_data();
	ssize_t bytesPP = tex.m_Bpp / 8;
	ssize_t mapLineSkip = tex.m_Width * bytesPP;

	for (ssize_t y = 0; y < terrainSize; ++y)
	{
		for (ssize_t x = 0; x < terrainSize; ++x)
		{
			int offset = y * mapLineSkip + x * bytesPP;

			// pick color channel with highest value
			u16 value = std::max(mapdata[offset+bytesPP*2], std::max(mapdata[offset], mapdata[offset+bytesPP]));

			heightmap[(terrainSize-y-1) * hmSize + x] = clamp(value * 256, 0, 65535);
		}
	}

	// update simulation
	CmpPtr<ICmpTerrain> cmpTerrain(*g_Game->GetSimulation2(), SYSTEM_ENTITY);
	if (cmpTerrain) cmpTerrain->ReloadTerrain();
	g_Game->GetView()->GetLOSTexture().MakeDirty();
}

MESSAGEHANDLER(SaveMap)
{
	CMapWriter writer;
	VfsPath pathname = VfsPath(*msg->filename).ChangeExtension(L".pmp");
	writer.SaveMap(pathname,
		g_Game->GetWorld()->GetTerrain(),
		g_Renderer.GetWaterManager(), g_Renderer.GetSkyManager(),
		&g_LightEnv, g_Game->GetView()->GetCamera(), g_Game->GetView()->GetCinema(),
		&g_Renderer.GetPostprocManager(),
		g_Game->GetSimulation2());
}

QUERYHANDLER(GetMapSettings)
{
	msg->settings = g_Game->GetSimulation2()->GetMapSettingsString();
}

BEGIN_COMMAND(SetMapSettings)
{
	std::string m_OldSettings, m_NewSettings;

	void SetSettings(const std::string& settings)
	{
		g_Game->GetSimulation2()->SetMapSettings(settings);
	}

	void Do()
	{
		m_OldSettings = g_Game->GetSimulation2()->GetMapSettingsString();
		m_NewSettings = *msg->settings;

		SetSettings(m_NewSettings);
	}

	// TODO: we need some way to notify the Atlas UI when the settings are changed
	//	externally, otherwise this will have no visible effect
	void Undo()
	{
		// SetSettings(m_OldSettings);
	}

	void Redo()
	{
		// SetSettings(m_NewSettings);
	}

	void MergeIntoPrevious(cSetMapSettings* prev)
	{
		prev->m_NewSettings = m_NewSettings;
	}
};
END_COMMAND(SetMapSettings)

MESSAGEHANDLER(LoadPlayerSettings)
{
	g_Game->GetSimulation2()->LoadPlayerSettings(msg->newplayers);
}

QUERYHANDLER(GetMapSizes)
{
	msg->sizes = g_Game->GetSimulation2()->GetMapSizes();
}

QUERYHANDLER(GetMiniMapDisplay)
{
	const CTerrain* terrain = g_Game->GetWorld()->GetTerrain();
	const ssize_t dimension = terrain->GetVerticesPerSide() - 1;
	const ssize_t bpp = 24;
	const ssize_t buf_size = dimension * dimension * (bpp / 8);

	// Data is destined for a wxImage, which uses free.
	unsigned char* img = static_cast<unsigned char*>(malloc(buf_size));
	if (img)
	{
		// Stolen from MiniMap.cpp
		float shallowPassageHeight = 0.0f;
		// Get the maximum height for unit passage in water.
		CParamNode externalParamNode;
		CParamNode::LoadXML(externalParamNode, L"simulation/data/pathfinder.xml", "pathfinder");
		const CParamNode pathingSettings = externalParamNode.GetChild("Pathfinder").GetChild("PassabilityClasses");
		if (pathingSettings.GetChild("default").IsOk() && pathingSettings.GetChild("default").GetChild("MaxWaterDepth").IsOk())
			shallowPassageHeight = pathingSettings.GetChild("default").GetChild("MaxWaterDepth").ToFloat();

		ssize_t w = dimension;
		ssize_t h = dimension;
		float waterHeight = g_Renderer.GetWaterManager()->m_WaterHeight;

		for (ssize_t j = 0; j < h; ++j)
		{
			// Work backwards to vertically flip the image.
			unsigned char* dataPtr = img + 3 * (h - j - 1) * dimension;
			for (ssize_t i = 0; i < w; ++i)
			{
				float avgHeight = (terrain->GetVertexGroundLevel(i, j)
					+ terrain->GetVertexGroundLevel(i + 1, j)
					+ terrain->GetVertexGroundLevel(i, j + 1)
					+ terrain->GetVertexGroundLevel(i + 1, j + 1)
					) / 4.0f;

				if (avgHeight < waterHeight && avgHeight > waterHeight - shallowPassageHeight)
				{
					// shallow water
					*dataPtr++ = 0x70;
					*dataPtr++ = 0x98;
					*dataPtr++ = 0xc0;
				}
				else if (avgHeight < waterHeight)
				{
					// Set water as constant color for consistency on different maps
					*dataPtr++ = 0x50;
					*dataPtr++ = 0x78;
					*dataPtr++ = 0xa0;
				}
				else
				{
					int hmap = ((int)terrain->GetHeightMap()[j * dimension + i]) >> 8;
					float scale = float((hmap / 3) + 170) / 255.0f;

					u32 color = 0xFFFFFFFF;

					CMiniPatch* mp = terrain->GetTile(i, j);
					if (mp)
					{
						CTerrainTextureEntry* tex = mp->GetTextureEntry();
						if (tex)
						{
							color = tex->GetBaseColor();
						}
					}

					// Convert 
					*dataPtr++ = unsigned char(float(color & 0xff) * scale);
					*dataPtr++ = unsigned char(float((color >> 8) & 0xff) * scale);
					*dataPtr++ = unsigned char(float((color >> 16) & 0xff) * scale);
				}
			}
		}
	}
		
	msg->imageBytes = static_cast<void*>(img);
	msg->dimension = dimension;
}

QUERYHANDLER(GetRMSData)
{
	msg->data = g_Game->GetSimulation2()->GetRMSData();
}

QUERYHANDLER(GetCurrentMapSize)
{
	msg->size = g_Game->GetWorld()->GetTerrain()->GetTilesPerSide();
}

BEGIN_COMMAND(ResizeMap)
{
	int m_OldTiles, m_NewTiles;

	cResizeMap()
	{
	}

	void MakeDirty()
	{
		CmpPtr<ICmpTerrain> cmpTerrain(*g_Game->GetSimulation2(), SYSTEM_ENTITY);
		if (cmpTerrain)
			cmpTerrain->ReloadTerrain();

		// The LOS texture won't normally get updated when running Atlas
		// (since there's no simulation updates), so explicitly dirty it
		g_Game->GetView()->GetLOSTexture().MakeDirty();
	}

	void ResizeTerrain(int tiles)
	{
		CTerrain* terrain = g_Game->GetWorld()->GetTerrain();

		terrain->Resize(tiles / PATCH_SIZE);

		MakeDirty();
	}

	void Do()
	{
		CmpPtr<ICmpTerrain> cmpTerrain(*g_Game->GetSimulation2(), SYSTEM_ENTITY);
		if (!cmpTerrain)
		{
			m_OldTiles = m_NewTiles = 0;
		}
		else
		{
			m_OldTiles = (int)cmpTerrain->GetTilesPerSide();
			m_NewTiles = msg->tiles;
		}

		ResizeTerrain(m_NewTiles);
	}

	void Undo()
	{
		ResizeTerrain(m_OldTiles);
	}

	void Redo()
	{
		ResizeTerrain(m_NewTiles);
	}
};
END_COMMAND(ResizeMap)

QUERYHANDLER(VFSFileExists)
{
	msg->exists = VfsFileExists(*msg->path);
}

static Status AddToFilenames(const VfsPath& pathname, const CFileInfo& UNUSED(fileInfo), const uintptr_t cbData)
{
	std::vector<std::wstring>& filenames = *(std::vector<std::wstring>*)cbData;
	filenames.push_back(pathname.string().c_str());
	return INFO::OK;
}

QUERYHANDLER(GetMapList)
{
	std::vector<std::wstring> scenarioFilenames;
	vfs::ForEachFile(g_VFS, L"maps/scenarios/", AddToFilenames, (uintptr_t)&scenarioFilenames, L"*.xml", vfs::DIR_RECURSIVE);
	msg->scenarioFilenames = scenarioFilenames;

	std::vector<std::wstring> skirmishFilenames;
	vfs::ForEachFile(g_VFS, L"maps/skirmishes/", AddToFilenames, (uintptr_t)&skirmishFilenames, L"*.xml", vfs::DIR_RECURSIVE);
	msg->skirmishFilenames = skirmishFilenames;
}

}

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

#include "PsuedoMiniMapPanel.h"

#include "GameInterface/Messages.h"
#include "GameInterface/MessagePasser.h"
#include "ScenarioEditor/Tools/Common/Tools.h"

#include <math.h>
#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>
#include <wx/defs.h> 

namespace
{
	const int PanelRadius = 64 + 1;
	const wxPoint PanelCenter = wxPoint(PanelRadius + 1, PanelRadius + 1);
	const char* ScreenToneColor[] =
	{
		/* columns rows colors chars-per-pixel */
		"8 8 2 1",
		"O c White",
		"X c Black",
		/* pixels */
		"OOOOOOOO",
		"OXXOOXXO",
		"OXXOOXXO",
		"OOOOOOOO",
		"OOOOOOOO",
		"OXXOOXXO",
		"OXXOOXXO",
		"OOOOOOOO"
	};
	const wxBitmap ScreenToneMask(ScreenToneColor);
	const wxPen ScreenTone = wxPen(ScreenToneMask, PanelRadius);
	const wxPoint ScreenToneOffset(-2 * PanelRadius, -2 * PanelRadius);
	const wxPen Rim(*wxBLACK, 3);
	const wxPen BackgroundMask(*wxBLACK, 2 * PanelRadius);
	const wxPen BorderPen(*wxWHITE, 2);

	bool Within(const wxPoint& test, const wxPoint& center, int radius)
	{
		int dx = abs(test.x - center.x);
		if (dx > radius)
			return false;
		int dy = abs(test.y - center.y);
		if (dy > radius)
			return false;
		if (dx + dy <= radius)
			return true;
		return (dx * dx + dy * dy <= radius * radius);
	}
}

PsuedoMiniMapPanel::PsuedoMiniMapPanel(wxWindow* parent, int currentSize)
	: wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(PanelRadius * 2 + 1, PanelRadius * 2 + 1)),
	m_CurrentSize(currentSize),
	m_LastMousePos(-1, -1), m_Dragging(false),
	m_SelectionRadius(PanelRadius), m_SelectionCenter(PanelCenter), m_SameOrGrowing(true), m_NewSize(currentSize)
{

	AtlasMessage::qGetMiniMapDisplay qryMiniMap;
	qryMiniMap.Post();
	int dim = qryMiniMap.dimension;
	unsigned char* data = static_cast<unsigned char*>((void*)qryMiniMap.imageBytes);
	
	wxImage miniMap = wxImage(dim, dim, data);
	miniMap.Rescale(PanelRadius * 2 + 1, PanelRadius * 2 + 1, wxIMAGE_QUALITY_BOX_AVERAGE);
	m_MiniMap = wxBitmap(miniMap);

	SetBackgroundStyle(wxBG_STYLE_PAINT);
}

wxPoint PsuedoMiniMapPanel::GetOffset() const 
{
	// Since offset is from center, amplitude is (at most) half the largest size.
	int size = std::max(m_CurrentSize, m_NewSize) / 2;
	// If the map is growing, the display is opposite what the actual offset is.
	float scalar = (m_SameOrGrowing ? 1.0 : -1.0) / PanelRadius * size;
	// Rebase offsets to center.
	int hOffset = m_SelectionCenter.x - PanelCenter.x;
	int vOffset = m_SelectionCenter.y - PanelCenter.y;
	return wxPoint(scalar * hOffset, scalar * vOffset);
}

void PsuedoMiniMapPanel::OnNewSize(wxCommandEvent& evt)
{
	if (!evt.IsSelection())
		return;

	evt.Skip();

	m_NewSize = wxAtoi(static_cast<wxStringClientData*>(evt.GetClientObject())->GetData());

	m_SameOrGrowing = m_NewSize >= m_CurrentSize;
	m_SelectionRadius = double(std::min(m_NewSize, m_CurrentSize)) / std::max(m_NewSize, m_CurrentSize) * PanelRadius;

	Refresh();
}

void PsuedoMiniMapPanel::OnMouseDown(wxMouseEvent& evt)
{
	// Capture on button-down, so we can respond even when the mouse
	// moves off the window
	if (!m_Dragging && evt.ButtonDown() &&
		Within(evt.GetPosition(), PanelCenter, PanelRadius) &&
		Within(evt.GetPosition(), m_SelectionCenter, m_SelectionRadius))
	{
		m_LastMousePos = evt.GetPosition();
		m_Dragging = true;
	}
}

void PsuedoMiniMapPanel::OnMouseUp(wxMouseEvent& evt)
{
	if (m_Dragging &&
		!(evt.ButtonIsDown(wxMOUSE_BTN_LEFT) || evt.ButtonIsDown(wxMOUSE_BTN_MIDDLE) || evt.ButtonIsDown(wxMOUSE_BTN_RIGHT))
		)
	{
		m_Dragging = false;
	}
}

void PsuedoMiniMapPanel::OnMouseMove(wxMouseEvent& evt)
{
	if (m_Dragging && evt.Dragging())
	{
		if (m_LastMousePos == evt.GetPosition())
			return;

		wxPoint delta = evt.GetPosition() - m_LastMousePos;
		wxPoint moved = m_SelectionCenter + delta;

		if (!Within(moved, PanelCenter, PanelRadius))
			return;

		m_SelectionCenter = moved;
		m_LastMousePos = evt.GetPosition();
		Refresh();
	}
}

void PsuedoMiniMapPanel::PaintEvent(wxPaintEvent& WXUNUSED(evt))
{
	wxPaintDC dc(this);
	// Background must be grabbed from paint dc, not gc, or color may be transparent.
	wxColor background = dc.GetBackground().GetColour();

	// Manually double-buffering for transparent image pieces and to avoid flicker.
	wxImage image = wxImage(PanelRadius * 2 + 1, PanelRadius * 2 + 1, true);
	unsigned char* aData = new unsigned char[(PanelRadius * 2 + 1) * (PanelRadius * 2 + 1)];
	memset(aData, wxIMAGE_ALPHA_TRANSPARENT, (PanelRadius * 2 + 1) * (PanelRadius * 2 + 1));
	image.SetAlpha(aData);
	wxGraphicsContext* gc = wxGraphicsContext::Create(image);

	if (m_SameOrGrowing)
	{
		gc->SetBrush(*wxBLACK_BRUSH);
		gc->DrawRectangle(0, 0, PanelRadius * 2 + 1, PanelRadius * 2 + 1);
		gc->DrawBitmap(m_MiniMap, m_SelectionCenter.x - m_SelectionRadius, m_SelectionCenter.y - m_SelectionRadius, m_SelectionRadius * 2, m_SelectionRadius * 2);
		
		// Mask out minimap edges
		gc->SetBrush(*wxTRANSPARENT_BRUSH);
		gc->SetPen(BackgroundMask);
		gc->DrawEllipse(m_SelectionCenter.x - m_SelectionRadius - PanelRadius, m_SelectionCenter.y - m_SelectionRadius - PanelRadius, (m_SelectionRadius + PanelRadius) * 2, (m_SelectionRadius + PanelRadius) * 2);
		
		gc->SetPen(BorderPen); 
		gc->DrawEllipse(m_SelectionCenter.x - m_SelectionRadius, m_SelectionCenter.y - m_SelectionRadius, m_SelectionRadius * 2, m_SelectionRadius * 2);
	}
	else
	{
		wxImage tone = wxImage(8, 8, true);
		unsigned char* alphaData = new unsigned char[8 * 8];
		memset(alphaData, wxIMAGE_ALPHA_TRANSPARENT, 8 * 8);
		tone.SetAlpha(alphaData);
		wxGraphicsContext* d = wxGraphicsContext::Create(tone);
		d->SetBrush(*wxWHITE_BRUSH);
		d->DrawRectangle(0, 0, 8, 8);
		
		delete d;

		gc->DrawBitmap(m_MiniMap, 0, 0, PanelRadius * 2 + 1, PanelRadius * 2 + 1);
		// "fade out" trimmed areas by drawing a screentone ring ring.
		gc->SetBrush(*wxTRANSPARENT_BRUSH);
		wxPen p = wxPen(*wxGREEN, PanelRadius, wxPENSTYLE_STIPPLE_MASK_OPAQUE);
		wxBitmap t = wxBitmap(tone);
		t.SetMask(new wxMask(ScreenToneMask));
		p.SetStipple(t);
		gc->SetPen(p);
		gc->DrawEllipse(PanelRadius * .5, PanelRadius * .5, PanelRadius * 1.5, PanelRadius * 1.5);
		//gc->SetPen(tone);
		//gc->DrawEllipse(m_SelectionCenter.x - m_SelectionRadius - PanelRadius, m_SelectionCenter.y - m_SelectionRadius - PanelRadius, (m_SelectionRadius + PanelRadius) * 2, (m_SelectionRadius + PanelRadius) * 2);
	}

	// Centering markers.
	gc->SetBrush(*wxBLACK_BRUSH);
	gc->SetPen(*wxBLACK_PEN);
	gc->DrawEllipse(m_SelectionCenter.x - 2, m_SelectionCenter.y - 2, 2 * 2, 2 * 2);
	gc->SetPen(*wxWHITE_PEN);
	gc->StrokeLine(PanelRadius - 10, PanelRadius, PanelRadius + 10, PanelRadius);
	gc->StrokeLine(PanelRadius, PanelRadius + 10, PanelRadius, PanelRadius - 10);

	// Round border.
	gc->SetBrush(*wxTRANSPARENT_BRUSH);
	gc->SetPen(Rim);
	gc->DrawEllipse(1, 1, PanelRadius * 2 - 1, PanelRadius * 2 - 1);
	wxPen mask(background, PanelRadius);
	gc->SetPen(mask);
	gc->DrawEllipse(-PanelRadius, -PanelRadius, PanelRadius * 2 * 2, PanelRadius * 2 * 2);

	delete gc;

	dc.DrawBitmap(wxBitmap(image), 0, 0);
}

void PsuedoMiniMapPanel::EraseBackground(wxEraseEvent& WXUNUSED(evt))
{
	// Do nothing - don't erase to remove flicker.
}

BEGIN_EVENT_TABLE(PsuedoMiniMapPanel, wxPanel)
EVT_LEAVE_WINDOW(PsuedoMiniMapPanel::OnMouseUp)
EVT_LEFT_DOWN(PsuedoMiniMapPanel::OnMouseDown)
EVT_LEFT_UP(PsuedoMiniMapPanel::OnMouseUp)
EVT_RIGHT_DOWN(PsuedoMiniMapPanel::OnMouseDown)
EVT_RIGHT_UP(PsuedoMiniMapPanel::OnMouseUp)
EVT_MIDDLE_DOWN(PsuedoMiniMapPanel::OnMouseDown)
EVT_MIDDLE_UP(PsuedoMiniMapPanel::OnMouseUp)
EVT_MOTION(PsuedoMiniMapPanel::OnMouseMove)
EVT_PAINT(PsuedoMiniMapPanel::PaintEvent)
END_EVENT_TABLE()
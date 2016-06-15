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

#include "MapResizeDialog.h"
#include "ScenarioEditor/ScenarioEditor.h"

#include "GameInterface/MessagePasser.h"
#include "GameInterface/Messages.h"

#include <wx/statline.h>

MapResizeDialog::MapResizeDialog(wxWindow* parent, size_t currentSize)
	: wxDialog(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(600, 400), wxCAPTION | wxRESIZE_BORDER),
	m_NewSize(0)
{
	Freeze();

	// Load the map sizes list
	AtlasMessage::qGetMapSizes qrySizes;
	qrySizes.Post();
	AtObj sizes = AtlasObject::LoadFromJSON(*qrySizes.sizes);
	for (AtIter s = sizes["Data"]["item"]; s.defined(); ++s)
	{
		long tiles = 0;
		wxString(s["Tiles"]).ToLong(&tiles);
		m_SizeNames.Add(wxString(s["Name"]));
		m_SizeTiles.push_back((size_t)tiles);
	}

	SetTitle(_("Resize map"));

	wxSizer* sizer = new wxBoxSizer(wxVERTICAL);

	wxStaticText* label = new wxStaticText(this, wxID_ANY, _("Select new map size. WARNING: This probably only works reliably on blank maps."), wxDefaultPosition, wxDefaultSize);
	sizer->Add(label, wxSizerFlags().Align(wxALIGN_CENTER).Border(wxALL, 10));
	
	wxListBox* listBox = new wxListBox(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, 0, NULL, wxLB_SINGLE | wxLB_HSCROLL);
	listBox->Append(m_SizeNames);


	sizer->Add(new wxStaticLine(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLI_HORIZONTAL), wxSizerFlags().Expand().Border(wxRIGHT | wxLEFT, 7));
	sizer->AddSpacer(5);

	wxSizer* buttonSizer = new wxBoxSizer(wxHORIZONTAL);
	buttonSizer->Add(new wxButton(this, wxID_OK, _("OK")));
	buttonSizer->AddSpacer(5);
	buttonSizer->Add(new wxButton(this, wxID_CANCEL, _("Cancel")));

	sizer->Add(buttonSizer, wxSizerFlags().Align(wxALIGN_RIGHT | wxALIGN_BOTTOM).Border(wxRIGHT | wxBOTTOM, 10));

	SetSizerAndFit(sizer);
	Layout();
	Thaw();
}

size_t MapResizeDialog::GetNewSize() const
{
	return m_NewSize;
}

void MapResizeDialog::OnListBox(wxCommandEvent& evt)
{
	int index = evt.GetInt();
	if (index < 0 || index >= m_SizeTiles.size())
		return;

	m_NewSize = m_SizeTiles[index];
	
	if (evt.GetEventType() == wxEVT_COMMAND_LISTBOX_DOUBLECLICKED)
	{
		EndModal(wxID_OK);
	}
}

void MapResizeDialog::OnCancel(wxCommandEvent& WXUNUSED(evt))
{
	EndModal(wxID_CANCEL);
}

void MapResizeDialog::OnOK(wxCommandEvent& WXUNUSED(evt))
{
	EndModal(wxID_OK);
}

BEGIN_EVENT_TABLE(MapResizeDialog, wxDialog)
	EVT_BUTTON(wxID_CANCEL, MapResizeDialog::OnCancel)
	EVT_BUTTON(wxID_OK, MapResizeDialog::OnOK)
	EVT_LISTBOX(wxID_ANY, MapResizeDialog::OnListBox)
	EVT_LISTBOX_DCLICK(wxID_ANY, MapResizeDialog::OnListBox)
END_EVENT_TABLE()
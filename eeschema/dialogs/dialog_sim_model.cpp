/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2022 Mikolaj Wielgus
 * Copyright (C) 2022 KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you may find one here:
 * https://www.gnu.org/licenses/gpl-3.0.html
 * or you may search the http://www.gnu.org website for the version 3 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

#include <dialog_sim_model.h>
#include <sim/sim_property.h>
#include <sim/sim_library_kibis.h>
#include <sim/sim_library_spice.h>
#include <sim/sim_model_kibis.h>
#include <sim/sim_model_raw_spice.h>
#include <widgets/wx_grid.h>
#include <kiplatform/ui.h>
#include <confirm.h>
#include <string_utils.h>
#include <locale_io.h>
#include <wx/filedlg.h>
#include <wx/textfile.h>

using CATEGORY = SIM_MODEL::PARAM::CATEGORY;


template <typename T>
DIALOG_SIM_MODEL<T>::DIALOG_SIM_MODEL( wxWindow* aParent, SCH_SYMBOL& aSymbol,
                                       std::vector<T>& aFields )
    : DIALOG_SIM_MODEL_BASE( aParent ),
      m_symbol( aSymbol ),
      m_fields( aFields ),
      m_library( std::make_shared<SIM_LIBRARY_SPICE>() ),
      m_prevModel( nullptr ),
      m_firstCategory( nullptr ),
      m_prevParamGridSelection( nullptr ),
      m_wasCodePreviewUpdated( true )
{
    m_modelNameCombobox->SetValidator( m_modelNameValidator );
    m_browseButton->SetBitmap( KiBitmap( BITMAPS::small_folder ) );

    m_sortedSymbolPins = m_symbol.GetLibPins();
    std::sort( m_sortedSymbolPins.begin(), m_sortedSymbolPins.end(),
               []( const LIB_PIN* lhs, const LIB_PIN* rhs )
               {
                   // We sort by StrNumCmp because SIM_MODEL_BASE sorts with it too.
                   return StrNumCmp( lhs->GetNumber(), rhs->GetNumber(), true ) < 0;
               } );

    for( SIM_MODEL::TYPE type : SIM_MODEL::TYPE_ITERATOR() )
    {
        m_models.push_back( SIM_MODEL::Create( type, m_sortedSymbolPins.size() ) );

        SIM_MODEL::DEVICE_TYPE_ deviceType = SIM_MODEL::TypeInfo( type ).deviceType;

        if( !m_curModelTypeOfDeviceType.count( deviceType ) )
            m_curModelTypeOfDeviceType[deviceType] = type;
    }


    m_typeChoice->Clear();

    for( SIM_MODEL::DEVICE_TYPE_ deviceType : SIM_MODEL::DEVICE_TYPE__ITERATOR() )
        m_deviceTypeChoice->Append( SIM_MODEL::DeviceTypeInfo( deviceType ).description );

    m_scintillaTricks = std::make_unique<SCINTILLA_TRICKS>( m_codePreview, wxT( "{}" ), false );

    m_paramGridMgr->Bind( wxEVT_PG_SELECTED, &DIALOG_SIM_MODEL::onParamGridSelectionChange, this );

    m_paramGrid->SetValidationFailureBehavior( wxPG_VFB_STAY_IN_PROPERTY
                                               | wxPG_VFB_BEEP
                                               | wxPG_VFB_MARK_CELL );

    m_paramGrid->SetColumnProportion( static_cast<int>( PARAM_COLUMN::DESCRIPTION ), 50 );
    m_paramGrid->SetColumnProportion( static_cast<int>( PARAM_COLUMN::VALUE ), 18 );
    m_paramGrid->SetColumnProportion( static_cast<int>( PARAM_COLUMN::UNIT ), 10 );
    m_paramGrid->SetColumnProportion( static_cast<int>( PARAM_COLUMN::DEFAULT ), 12 );
    m_paramGrid->SetColumnProportion( static_cast<int>( PARAM_COLUMN::TYPE ), 10 );

    if( wxPropertyGrid* grid = m_paramGrid->GetGrid() )
    {
        //grid->SetCellBackgroundColour( grid->GetPropertyDefaultCell().GetBgCol() );
        //grid->SetCellTextColour( grid->GetPropertyDefaultCell().GetFgCol();

        // In wx 3.0 the color will be wrong sometimes.
        grid->SetCellDisabledTextColour( wxSystemSettings::GetColour( wxSYS_COLOUR_GRAYTEXT ) );

        grid->Bind( wxEVT_SET_FOCUS, &DIALOG_SIM_MODEL::onParamGridSetFocus, this );

        grid->AddActionTrigger( wxPG_ACTION_EDIT, WXK_RETURN );
        grid->DedicateKey( WXK_RETURN );
        grid->AddActionTrigger( wxPG_ACTION_NEXT_PROPERTY, WXK_RETURN );

        grid->DedicateKey( WXK_UP );
        grid->DedicateKey( WXK_DOWN );
    }
    else
        wxFAIL;

    // Now all widgets have the size fixed, call FinishDialogSettings
    finishDialogSettings();
    setIbisMode( IsIbisLoaded() );
}


template <typename T>
bool DIALOG_SIM_MODEL<T>::TransferDataToWindow()
{
    wxString libraryFilename = SIM_MODEL::GetFieldValue( &m_fields, SIM_LIBRARY::LIBRARY_FIELD );

    if( libraryFilename != "" )
    {
        // The model is sourced from a library, optionally with instance overrides.
        loadLibrary( libraryFilename );

        bool ibisMode = IsIbisLoaded();
        setIbisMode( ibisMode );

        // Must be set before curModel() is used since the latter checks the combobox value.
        m_modelNameCombobox->SetStringSelection(
                SIM_MODEL::GetFieldValue( &m_fields, SIM_LIBRARY::NAME_FIELD ) );

        if( ibisMode && ( m_modelNameCombobox->GetSelection() >= 0 ) )
        {
            std::shared_ptr<SIM_MODEL_KIBIS> kibismodel =
                    std::dynamic_pointer_cast<SIM_MODEL_KIBIS>(
                            m_libraryModels.at( m_modelNameCombobox->GetSelection() ) );

            if( kibismodel )
            {
                wxCommandEvent dummyEvent;
                onModelNameCombobox( dummyEvent ); // refresh list of pins

                long unsigned int i = 0;

                for( std::pair<std::string, std::string> strs : kibismodel->GetIbisPins() )
                {
                    if( strs.first
                        == SIM_MODEL::GetFieldValue( &m_fields, SIM_LIBRARY_KIBIS::PIN_FIELD ) )
                    {
                        kibismodel->ChangePin(
                                *( std::dynamic_pointer_cast<SIM_LIBRARY_KIBIS>( m_library ) ),
                                strs.first );
                        m_ibisPinCombobox->SetSelection( i );
                        break;
                    }
                    i++;
                }

                if( i < kibismodel->GetIbisPins().size() )
                {
                    onIbisPinCombobox( dummyEvent ); // refresh list of models

                    m_ibisModelCombobox->SetStringSelection(
                            SIM_MODEL::GetFieldValue( &m_fields, SIM_LIBRARY_KIBIS::MODEL_FIELD ) );
                }
            }
        }

        m_excludeSymbolCheckbox->SetValue( !curModel().IsEnabled() );
    }
    else
    {
        // The model is sourced from the instance.
        SIM_MODEL::TYPE type = SIM_MODEL::ReadTypeFromFields( m_fields );

        try
        {
            m_models.at( static_cast<int>( SIM_MODEL::ReadTypeFromFields( m_fields ) ) ) =
                    SIM_MODEL::Create( m_sortedSymbolPins.size(), m_fields );
        }
        catch( const IO_ERROR& e )
        {
            DisplayErrorMessage( this, e.What() );
            return DIALOG_SIM_MODEL_BASE::TransferDataToWindow();
        }

        m_curModelType = type;
    }

    m_overrideCheckbox->SetValue( curModel().HasNonInstanceOverrides() );

    updateWidgets();

    return DIALOG_SIM_MODEL_BASE::TransferDataToWindow();
}


template <typename T>
bool DIALOG_SIM_MODEL<T>::TransferDataFromWindow()
{
    m_pinAssignmentsGrid->CommitPendingChanges();

    if( !DIALOG_SIM_MODEL_BASE::TransferDataFromWindow() )
        return false;

    std::string modelName;

    modelName = m_modelNameCombobox->GetValue();

    SIM_MODEL::SetFieldValue( m_fields, SIM_LIBRARY::NAME_FIELD, modelName );

    std::string path;

    if( m_useLibraryModelRadioButton->GetValue() || IsIbisLoaded() )
    {
        path = m_library->GetFilePath();
        wxFileName fn( path );

        if( fn.MakeRelativeTo( Prj().GetProjectPath() ) && !fn.GetFullPath().StartsWith( ".." ) )
            path = fn.GetFullPath();
    }

    SIM_MODEL::SetFieldValue( m_fields, SIM_LIBRARY::LIBRARY_FIELD, path );

    if( IsIbisLoaded() )
    {
        std::shared_ptr<SIM_MODEL_KIBIS> kibismodel = std::dynamic_pointer_cast<SIM_MODEL_KIBIS>(
                m_libraryModels.at( m_modelNameCombobox->GetSelection() ) );

        if( kibismodel )

        {
            SIM_MODEL::SetFieldValue(
                    m_fields, SIM_LIBRARY_KIBIS::PIN_FIELD,
                    kibismodel->GetIbisPins().at( m_ibisPinCombobox->GetSelection() ).first );
            SIM_MODEL::SetFieldValue( m_fields, SIM_LIBRARY_KIBIS::MODEL_FIELD,
                                      std::string( m_ibisModelCombobox->GetValue().c_str() ) );
        }
    }

    curModel().WriteFields( m_fields );

    return true;
}


template <typename T>
void DIALOG_SIM_MODEL<T>::updateWidgets()
{
    updateModelParamsTab();
    updateModelCodeTab();
    updatePinAssignments();

    m_prevModel = &curModel();
}


template <typename T>
void DIALOG_SIM_MODEL<T>::updateModelParamsTab()
{
    if( ( &curModel() != m_prevModel ) || curModel().RequiresUIUpdate() )
    {
        SIM_MODEL::DEVICE_TYPE_ deviceType = SIM_MODEL::TypeInfo( curModel().GetType() ).deviceType;

        // Change the Type choice to match the current device type.
        if( !m_prevModel || deviceType != m_prevModel->GetDeviceType()
            || curModel().RequiresUIUpdate() )
        {
            curModel().UIUpdated();
            m_deviceTypeChoice->SetSelection( static_cast<int>( deviceType ) );

            m_typeChoice->Clear();

            for( SIM_MODEL::TYPE type : SIM_MODEL::TYPE_ITERATOR() )
            {
                if( SIM_MODEL::TypeInfo( type ).deviceType == deviceType )
                {
                    wxString description = SIM_MODEL::TypeInfo( type ).description;

                    if( !description.IsEmpty() )
                        m_typeChoice->Append( description );

                    if( type == curModel().GetType() )
                        m_typeChoice->SetSelection( m_typeChoice->GetCount() - 1 );
                }
            }
        }

        // This wxPropertyGridManager column and header stuff has to be here because it segfaults in
        // the constructor.

        m_paramGridMgr->SetColumnCount( static_cast<int>( PARAM_COLUMN::END_ ) );

        m_paramGridMgr->SetColumnTitle( static_cast<int>( PARAM_COLUMN::UNIT ), "Unit" );
        m_paramGridMgr->SetColumnTitle( static_cast<int>( PARAM_COLUMN::DEFAULT ), "Default" );
        m_paramGridMgr->SetColumnTitle( static_cast<int>( PARAM_COLUMN::TYPE ), "Type" );

        m_paramGridMgr->ShowHeader();


        m_paramGrid->Clear();

        m_firstCategory = m_paramGrid->Append( new wxPropertyCategory( "Geometry" ) );
        m_paramGrid->HideProperty( "Geometry" );

        m_paramGrid->Append( new wxPropertyCategory( "AC" ) );
        m_paramGrid->HideProperty( "AC" );

        m_paramGrid->Append( new wxPropertyCategory( "DC" ) );
        m_paramGrid->HideProperty( "DC" );

        m_paramGrid->Append( new wxPropertyCategory( "Capacitance" ) );
        m_paramGrid->HideProperty( "Capacitance" );

        m_paramGrid->Append( new wxPropertyCategory( "Temperature" ) );
        m_paramGrid->HideProperty( "Temperature" );

        m_paramGrid->Append( new wxPropertyCategory( "Noise" ) );
        m_paramGrid->HideProperty( "Noise" );

        m_paramGrid->Append( new wxPropertyCategory( "Distributed Quantities" ) );
        m_paramGrid->HideProperty( "Distributed Quantities" );

        m_paramGrid->Append( new wxPropertyCategory( "Waveform" ) );
        m_paramGrid->HideProperty( "Waveform" );

        m_paramGrid->Append( new wxPropertyCategory( "Limiting Values" ) );
        m_paramGrid->HideProperty( "Limiting Values" );

        m_paramGrid->Append( new wxPropertyCategory( "Advanced" ) );
        m_paramGrid->HideProperty( "Advanced" );

        m_paramGrid->Append( new wxPropertyCategory( "Flags" ) );
        m_paramGrid->HideProperty( "Flags" );

        m_paramGrid->CollapseAll();

        for( int i = 0; i < curModel().GetParamCount(); ++i )
            addParamPropertyIfRelevant( i );

        m_paramGrid->CollapseAll();
        m_paramGrid->Expand( "AC" );
        m_paramGrid->Expand( "Waveform" );
    }

    // Either enable all properties or disable all except the principal ones.
    // Set all properties to default colors.
    for( wxPropertyGridIterator it = m_paramGrid->GetIterator(); !it.AtEnd(); ++it )
    {
        SIM_PROPERTY* prop = dynamic_cast<SIM_PROPERTY*>( *it );
        
        if( !prop )
            continue;

        wxColour bgCol = m_paramGrid->GetGrid()->GetPropertyDefaultCell().GetBgCol();
        wxColour fgCol = m_paramGrid->GetGrid()->GetPropertyDefaultCell().GetFgCol();

        for( int col = 0; col < m_paramGridMgr->GetColumnCount(); ++col )
        {
            ( *it )->GetCell( col ).SetBgCol( bgCol );
            ( *it )->GetCell( col ).SetFgCol( fgCol );
        }

        // Model values other than the currently edited value may have changed. Update them.
        // This feature is called "autofill" and present only in certain models. Don't do it for
        // models that don't have it for performance reasons.
        if( curModel().HasAutofill() )
            ( *it )->SetValueFromString( prop->GetParam().value->ToString() );

        // Most of the values are disabled when the override checkbox is unchecked.
        ( *it )->Enable(
                ( IsIbisLoaded() ) || m_useInstanceModelRadioButton->GetValue()
                || ( prop->GetParam().info.isInstanceParam
                     && prop->GetParam().info.category == SIM_MODEL::PARAM::CATEGORY::PRINCIPAL )
                || m_overrideCheckbox->GetValue() );
    }
}


template <typename T>
void DIALOG_SIM_MODEL<T>::updateModelCodeTab()
{
    wxString modelName = m_modelNameCombobox->GetStringSelection();

    if( m_useInstanceModelRadioButton->GetValue() || modelName.IsEmpty() )
        modelName = m_fields.at( REFERENCE_FIELD ).GetText();

    m_codePreview->SetEditable( true ); // ???

    if( dynamic_cast<SIM_MODEL_RAW_SPICE*>( &curModel() ) )
    {
        // For raw Spice models display the whole file instead.
        
        wxString path = curModel().FindParam( "lib" )->value->ToString();
        wxString absolutePath = Prj().AbsolutePath( path );
        wxTextFile file;
        wxString text;

        text << curModel().SpiceGenerator().Preview( std::string( modelName.ToUTF8() ) );
        text << "\n";
        text << "--- FILE SOURCE (" << path << ") ---\n";
        text << "\n";

        if( wxFileExists( absolutePath ) && file.Open( absolutePath ) )
        {
            for( text << file.GetFirstLine() << "\n";
                 !file.Eof();
                 text << file.GetNextLine() << "\n" )
            {
            }

            file.Close();
            m_codePreview->SetText( text );
        }
    }
    else
        m_codePreview->SetText( curModel().SpiceGenerator().Preview( std::string( modelName.ToUTF8() ) ) );

    m_codePreview->SetEditable( false ); // ???
    m_wasCodePreviewUpdated = true;
}


template <typename T>
void DIALOG_SIM_MODEL<T>::updatePinAssignments()
{
    removeOrphanedPinAssignments();

    // Reset the grid.

    m_pinAssignmentsGrid->ClearRows();
    m_pinAssignmentsGrid->AppendRows( static_cast<int>( m_sortedSymbolPins.size() ) );

    for( int row = 0; row < m_pinAssignmentsGrid->GetNumberRows(); ++row )
    {
        m_pinAssignmentsGrid->SetCellValue( row, static_cast<int>( PIN_COLUMN::MODEL ),
                                            "Not Connected" );
    }

    // Now set up the grid values in the Model column.
    for( int modelPinIndex = 0; modelPinIndex < curModel().GetPinCount(); ++modelPinIndex )
    {
        wxString symbolPinNumber = curModel().GetPin( modelPinIndex ).symbolPinNumber;

        if( symbolPinNumber == "" )
            continue;

        wxString modelPinString = getModelPinString( modelPinIndex );
        m_pinAssignmentsGrid->SetCellValue( findSymbolPinRow( symbolPinNumber ),
                                            static_cast<int>( PIN_COLUMN::MODEL ),
                                            modelPinString );
    }

    wxArrayString modelPinChoices = getModelPinChoices();

    // Set up the Symbol column grid values and Model column cell editors with dropdown options.
    for( int i = 0; i < m_pinAssignmentsGrid->GetNumberRows(); ++i )
    {
        wxString symbolPinString = getSymbolPinString( i );

        m_pinAssignmentsGrid->SetReadOnly( i, static_cast<int>( PIN_COLUMN::SYMBOL ) );
        m_pinAssignmentsGrid->SetCellValue( i, static_cast<int>( PIN_COLUMN::SYMBOL ),
                                            symbolPinString );

        wxString curModelPinString = m_pinAssignmentsGrid->GetCellValue(
                i, static_cast<int>( PIN_COLUMN::MODEL ) );

        wxArrayString actualModelPinChoices( modelPinChoices );

        if( curModelPinString != _( "Not Connected" ) )
            actualModelPinChoices.Insert( curModelPinString, 0 );

        // Using `new` here shouldn't cause a memory leak because `SetCellEditor()` calls
        // `DecRef()` on its last editor.
        m_pinAssignmentsGrid->SetCellEditor( i, static_cast<int>( PIN_COLUMN::MODEL ),
                                             new wxGridCellChoiceEditor( actualModelPinChoices ) );
    }

    // TODO: Show a preview of the symbol with the pin numbers shown.
    // TODO: Maybe show a preview of the code for subcircuits and code models.
}


template <typename T>
void DIALOG_SIM_MODEL<T>::removeOrphanedPinAssignments()
{
    for( int i = 0; i < curModel().GetPinCount(); ++i )
    {
        const SIM_MODEL::PIN& modelPin   = curModel().GetPin( i );
        bool                  isOrphaned = true;

        for( const LIB_PIN* symbolPin : m_sortedSymbolPins )
        {
            if( modelPin.symbolPinNumber == symbolPin->GetNumber() )
            {
                isOrphaned = false;
                break;
            }
        }

        if( isOrphaned )
            curModel().SetPinSymbolPinNumber( i, "" );
    }
}


template <typename T>
void DIALOG_SIM_MODEL<T>::loadLibrary( const wxString& aFilePath )
{
    const wxString absolutePath = Prj().AbsolutePath( aFilePath );

    if( absolutePath.EndsWith( ".ibs" ) )
        m_library = std::make_shared<SIM_LIBRARY_KIBIS>();
    else
        m_library = std::make_shared<SIM_LIBRARY_SPICE>();

    setIbisMode( IsIbisLoaded() );

    try
    {
        m_library->ReadFile( std::string( absolutePath.ToUTF8() ) );
    }
    catch( const IO_ERROR& e )
    {
        DisplayErrorMessage( this, wxString::Format( _( "Failed reading model library '%s'." ),
                                                     absolutePath ),
                             e.What() );
        return;
    }

    m_tclibraryPathName->ChangeValue( aFilePath );

    m_libraryModels.clear();

    try
    {
        for( unsigned i = 0; i < m_library->GetModels().size(); ++i )
        {
            const SIM_MODEL& baseModel = m_library->GetModels().at( i );
            wxString         baseModelName = m_library->GetModelNames().at( i );
            wxString         expectedModelName;

            expectedModelName =
                    SIM_MODEL::GetFieldValue( &m_fields, SIM_LIBRARY_KIBIS::NAME_FIELD );

            // Only the current model is initialized from fields. Others have default
            // initialization.
            if( baseModelName == expectedModelName )
            {
                //TODO: it's not cur model.

                m_libraryModels.push_back(
                        SIM_MODEL::Create( baseModel, m_sortedSymbolPins.size(), m_fields ) );
            }
            else
            {
                m_libraryModels.push_back( SIM_MODEL::Create( baseModel, m_sortedSymbolPins.size() ) );
            }
        }
    }
    catch( const IO_ERROR& e )
    {
        DisplayErrorMessage( this, e.What() );
    }

    wxArrayString modelNames;
    for( const std::string& name : m_library->GetModelNames() )
        modelNames.Add( name );

    auto validator = dynamic_cast<MODEL_NAME_VALIDATOR*>( m_modelNameCombobox->GetValidator() );

    if( validator )
        validator->SetIncludes( modelNames );

    m_modelNameCombobox->Set( modelNames );

    m_useLibraryModelRadioButton->SetValue( true );
}


template <typename T>
void DIALOG_SIM_MODEL<T>::addParamPropertyIfRelevant( int aParamIndex )
{
    if( curModel().GetParam( aParamIndex ).info.dir == SIM_MODEL::PARAM::DIR_OUT )
        return;

    switch( curModel().GetParam( aParamIndex ).info.category )
    {
    case CATEGORY::AC:
        m_paramGrid->HideProperty( "AC", false );
        m_paramGrid->AppendIn( "AC", newParamProperty( aParamIndex ) );
        break;

    case CATEGORY::DC:
        m_paramGrid->HideProperty( "DC", false );
        m_paramGrid->AppendIn( "DC", newParamProperty( aParamIndex ) );
        break;

    case CATEGORY::CAPACITANCE:
        m_paramGrid->HideProperty( "Capacitance", false );
        m_paramGrid->AppendIn( "Capacitance", newParamProperty( aParamIndex ) );
        break;

    case CATEGORY::TEMPERATURE:
        m_paramGrid->HideProperty( "Temperature", false );
        m_paramGrid->AppendIn( "Temperature", newParamProperty( aParamIndex ) );
        break;

    case CATEGORY::NOISE:
        m_paramGrid->HideProperty( "Noise", false );
        m_paramGrid->AppendIn( "Noise", newParamProperty( aParamIndex ) );
        break;

    case CATEGORY::DISTRIBUTED_QUANTITIES:
        m_paramGrid->HideProperty( "Distributed Quantities", false );
        m_paramGrid->AppendIn( "Distributed Quantities", newParamProperty( aParamIndex ) );
        break;

    case CATEGORY::WAVEFORM:
        m_paramGrid->HideProperty( "Waveform", false );
        m_paramGrid->AppendIn( "Waveform", newParamProperty( aParamIndex ) );
        break;

    case CATEGORY::GEOMETRY:
        m_paramGrid->HideProperty( "Geometry", false );
        m_paramGrid->AppendIn( "Geometry", newParamProperty( aParamIndex ) );
        break;

    case CATEGORY::LIMITING_VALUES:
        m_paramGrid->HideProperty( "Limiting Values", false );
        m_paramGrid->AppendIn( "Limiting Values", newParamProperty( aParamIndex ) );
        break;

    case CATEGORY::ADVANCED:
        m_paramGrid->HideProperty( "Advanced", false );
        m_paramGrid->AppendIn( "Advanced", newParamProperty( aParamIndex ) );
        break;

    case CATEGORY::FLAGS:
        m_paramGrid->HideProperty( "Flags", false );
        m_paramGrid->AppendIn( "Flags", newParamProperty( aParamIndex ) );
        break;

    default:
        m_paramGrid->Insert( m_firstCategory, newParamProperty( aParamIndex ) );
        break;

    case CATEGORY::INITIAL_CONDITIONS:
    case CATEGORY::SUPERFLUOUS:
        return;
    }
}

template <typename T>
wxPGProperty* DIALOG_SIM_MODEL<T>::newParamProperty( int aParamIndex ) const
{
    const SIM_MODEL::PARAM& param = curModel().GetParam( aParamIndex );
    wxString paramDescription;

    if( param.info.description == "" )
        paramDescription = wxString::Format( "%s", param.info.name );
    else
        paramDescription = wxString::Format( "%s (%s)", param.info.description, param.info.name );

    wxPGProperty* prop = nullptr;

    switch( param.info.type )
    {
    case SIM_VALUE::TYPE_BOOL:
        // TODO.
        prop = new SIM_BOOL_PROPERTY( paramDescription, param.info.name, m_library,
                                      curModelSharedPtr(), aParamIndex );
        prop->SetAttribute( wxPG_BOOL_USE_CHECKBOX, true );
        break;

    case SIM_VALUE::TYPE_INT:
        prop = new SIM_STRING_PROPERTY( paramDescription, param.info.name, m_library,
                                        curModelSharedPtr(), aParamIndex, SIM_VALUE::TYPE_INT );
        break;

    case SIM_VALUE::TYPE_FLOAT:
        prop = new SIM_STRING_PROPERTY( paramDescription, param.info.name, m_library,
                                        curModelSharedPtr(), aParamIndex, SIM_VALUE::TYPE_FLOAT );
        break;

    //case TYPE_COMPLEX:
    //  break;

    case SIM_VALUE::TYPE_STRING:
        if( param.info.enumValues.empty() )
        {
            prop = new SIM_STRING_PROPERTY( paramDescription, param.info.name, m_library,
                    curModelSharedPtr(), aParamIndex, SIM_VALUE::TYPE_STRING );
        }
        else
        {
            prop = new SIM_ENUM_PROPERTY( paramDescription, param.info.name, m_library,
                                          curModelSharedPtr(), aParamIndex, SIM_VALUE::TYPE_STRING );
        }
        break;

    default:
        prop = new wxStringProperty( paramDescription, param.info.name );
        break;
    }

    prop->SetAttribute( wxPG_ATTR_UNITS, param.info.unit );

    // Legacy due to the way we extracted the parameters from Ngspice.
    if( param.isOtherVariant )
        prop->SetCell( 3, wxString( param.info.defaultValueOfOtherVariant ) );
    else
        prop->SetCell( 3, wxString( param.info.defaultValue ) );

    wxString typeStr;

    switch( param.info.type )
    {
    case SIM_VALUE::TYPE_BOOL:           typeStr = wxString( "Bool"           ); break;
    case SIM_VALUE::TYPE_INT:            typeStr = wxString( "Int"            ); break;
    case SIM_VALUE::TYPE_FLOAT:          typeStr = wxString( "Float"          ); break;
    case SIM_VALUE::TYPE_COMPLEX:        typeStr = wxString( "Complex"        ); break;
    case SIM_VALUE::TYPE_STRING:         typeStr = wxString( "String"         ); break;
    case SIM_VALUE::TYPE_BOOL_VECTOR:    typeStr = wxString( "Bool Vector"    ); break;
    case SIM_VALUE::TYPE_INT_VECTOR:     typeStr = wxString( "Int Vector"     ); break;
    case SIM_VALUE::TYPE_FLOAT_VECTOR:   typeStr = wxString( "Float Vector"   ); break;
    case SIM_VALUE::TYPE_COMPLEX_VECTOR: typeStr = wxString( "Complex Vector" ); break;
    }

    prop->SetCell( static_cast<int>( PARAM_COLUMN::TYPE ), typeStr );

    if( m_useLibraryModelRadioButton->GetValue()
        && !m_overrideCheckbox->GetValue()
        && !param.info.isInstanceParam )
    {
        prop->Enable( false );
    }

    return prop;
}


template <typename T>
int DIALOG_SIM_MODEL<T>::findSymbolPinRow( const wxString& aSymbolPinNumber ) const
{
    for( int row = 0; row < static_cast<int>( m_sortedSymbolPins.size() ); ++row )
    {
        LIB_PIN* pin = m_sortedSymbolPins[row];

        if( pin->GetNumber() == aSymbolPinNumber )
            return row;
    }

    return -1;
}


template <typename T>
SIM_MODEL& DIALOG_SIM_MODEL<T>::curModel() const
{
    return *curModelSharedPtr();
}


template <typename T>
std::shared_ptr<SIM_MODEL> DIALOG_SIM_MODEL<T>::curModelSharedPtr() const
{
    if( m_useLibraryModelRadioButton->GetValue()
        && m_modelNameCombobox->GetSelection() != wxNOT_FOUND )
        return m_libraryModels.at( m_modelNameCombobox->GetSelection() );
    else
        return m_models.at( static_cast<int>( m_curModelType ) );
}


template <typename T>
wxString DIALOG_SIM_MODEL<T>::getSymbolPinString( int symbolPinIndex ) const
{
    LIB_PIN* pin = m_sortedSymbolPins.at( symbolPinIndex );
    wxString number;
    wxString name;

    if( pin )
    {
        number = pin->GetShownNumber();
        name = pin->GetShownName();
    }

    LOCALE_IO toggle;

    if( name == "" )
        return wxString::Format( "%s", number );
    else
        return wxString::Format( "%s (%s)", number, name );
}


template <typename T>
wxString DIALOG_SIM_MODEL<T>::getModelPinString( int aModelPinIndex ) const
{
    const wxString& pinName = curModel().GetPin( aModelPinIndex ).name;

    LOCALE_IO toggle;

    if( pinName == "" )
        return wxString::Format( "%d", aModelPinIndex + 1, pinName );
    else
        return wxString::Format( "%d (%s)", aModelPinIndex + 1, pinName );
}


template <typename T>
int DIALOG_SIM_MODEL<T>::getModelPinIndex( const wxString& aModelPinString ) const
{
    if( aModelPinString == "Not Connected" )
        return SIM_MODEL::PIN::NOT_CONNECTED;

    int length = aModelPinString.Find( " " );

    if( length == wxNOT_FOUND )
        length = static_cast<int>( aModelPinString.Length() );

    long result = 0;
    aModelPinString.Mid( 0, length ).ToCLong( &result );

    return static_cast<int>( result - 1 );
}


template <typename T>
wxArrayString DIALOG_SIM_MODEL<T>::getModelPinChoices() const
{
    wxArrayString modelPinChoices;

    for( int i = 0; i < curModel().GetPinCount(); ++i )
    {
        const SIM_MODEL::PIN& modelPin = curModel().GetPin( i );

        if( modelPin.symbolPinNumber != "" )
            continue;

        modelPinChoices.Add( getModelPinString( i ) );
    }

    modelPinChoices.Add( _( "Not Connected" ) );
    return modelPinChoices;
}


template <typename T>
void DIALOG_SIM_MODEL<T>::onRadioButton( wxCommandEvent& aEvent )
{
    updateWidgets();
}


template <typename T>
void DIALOG_SIM_MODEL<T>::onBrowseButtonClick( wxCommandEvent& aEvent )
{
    wxFileDialog dlg( this, _( "Browse Models" ), Prj().GetProjectPath() );

    if( dlg.ShowModal() == wxID_CANCEL )
        return;

    wxString path = dlg.GetPath();
    wxFileName fn( path );

    if( fn.MakeRelativeTo( Prj().GetProjectPath() ) && !fn.GetFullPath().StartsWith( ".." ) )
        path = fn.GetFullPath();

    loadLibrary( path );
}


template <typename T>
void DIALOG_SIM_MODEL<T>::onModelNameCombobox( wxCommandEvent& aEvent )
{
    if( IsIbisLoaded() )
    {
        wxArrayString    pinLabels;
        SIM_MODEL_KIBIS* modelkibis = dynamic_cast<SIM_MODEL_KIBIS*>( &curModel() );

        if( !modelkibis )
        {
            wxFAIL;
            return;
        }

        for( std::pair<wxString, wxString> strs : modelkibis->GetIbisPins() )
        {
            pinLabels.Add( strs.first + " - " + strs.second );
        }

        m_ibisPinCombobox->Set( pinLabels );

        wxArrayString emptyArray;
        m_ibisModelCombobox->Set( emptyArray );
    }
    updateWidgets();
}


template <typename T>
void DIALOG_SIM_MODEL<T>::onModelNameComboboxKillFocus( wxFocusEvent& aEvent )
{
    m_modelNameCombobox->SetSelection(
        m_modelNameCombobox->FindString( m_modelNameCombobox->GetValue() ) );
    updateWidgets();
}


template <typename T>
void DIALOG_SIM_MODEL<T>::onModelNameComboboxTextEnter( wxCommandEvent& aEvent )
{
    m_modelNameCombobox->SetSelection(
        m_modelNameCombobox->FindString( m_modelNameCombobox->GetValue() ) );
    onModelNameCombobox( aEvent );
}


template <typename T>
void DIALOG_SIM_MODEL<T>::onIbisPinCombobox( wxCommandEvent& aEvent )
{
    if( IsIbisLoaded() )
    {
        wxArrayString modelLabels;

        SIM_MODEL_KIBIS* modelkibis = dynamic_cast<SIM_MODEL_KIBIS*>( &curModel() );

        if( !modelkibis )
        {
            wxFAIL;
            return;
        }

        std::vector<std::pair<std::string, std::string>> strs = modelkibis->GetIbisPins();
        modelkibis->ChangePin( *std::dynamic_pointer_cast<SIM_LIBRARY_KIBIS>( m_library ),
                               strs.at( m_ibisPinCombobox->GetSelection() ).first );

        for( wxString modelName : modelkibis->GetIbisModels() )
            modelLabels.Add( modelName );

        m_ibisModelCombobox->Set( modelLabels );
    }
    updateWidgets();
}


template <typename T>
void DIALOG_SIM_MODEL<T>::onIbisPinComboboxTextEnter( wxCommandEvent& aEvent )
{
    m_ibisPinCombobox->SetSelection(
            m_ibisPinCombobox->FindString( m_ibisPinCombobox->GetValue() ) );

    onIbisPinCombobox( aEvent );
}


template <typename T>
void DIALOG_SIM_MODEL<T>::onIbisModelCombobox( wxCommandEvent& aEvent )
{
    updateWidgets();
}


template <typename T>
void DIALOG_SIM_MODEL<T>::onIbisModelComboboxTextEnter( wxCommandEvent& aEvent )
{
    m_ibisModelCombobox->SetSelection(
            m_ibisModelCombobox->FindString( m_ibisModelCombobox->GetValue() ) );

    onIbisPinCombobox( aEvent );
}

template <typename T>
void DIALOG_SIM_MODEL<T>::onOverrideCheckbox( wxCommandEvent& aEvent )
{
    updateWidgets();
}


template <typename T>
void DIALOG_SIM_MODEL<T>::onDeviceTypeChoice( wxCommandEvent& aEvent )
{
    SIM_MODEL::DEVICE_TYPE_ deviceType =
        static_cast<SIM_MODEL::DEVICE_TYPE_>( m_deviceTypeChoice->GetSelection() );

    m_curModelType = m_curModelTypeOfDeviceType.at( deviceType );

    updateWidgets();
}


template <typename T>
void DIALOG_SIM_MODEL<T>::onTypeChoice( wxCommandEvent& aEvent )
{
    SIM_MODEL::DEVICE_TYPE_ deviceType =
        static_cast<SIM_MODEL::DEVICE_TYPE_>( m_deviceTypeChoice->GetSelection() );
    wxString typeDescription = m_typeChoice->GetString( m_typeChoice->GetSelection() );

    for( SIM_MODEL::TYPE type : SIM_MODEL::TYPE_ITERATOR() )
    {
        if( deviceType == SIM_MODEL::TypeInfo( type ).deviceType
            && typeDescription == SIM_MODEL::TypeInfo( type ).description )
        {
            if( IsIbisLoaded()
                && ( type == SIM_MODEL::TYPE::KIBIS_DEVICE
                     || type == SIM_MODEL::TYPE::KIBIS_DRIVER ) )
            {
                std::shared_ptr<SIM_MODEL_KIBIS> kibismodel =
                        std::dynamic_pointer_cast<SIM_MODEL_KIBIS>(
                                m_libraryModels.at( m_modelNameCombobox->GetSelection() ) );

                m_libraryModels.at( m_modelNameCombobox->GetSelection() ) =
                        std::shared_ptr<SIM_MODEL>( dynamic_cast<SIM_MODEL*>(
                                new SIM_MODEL_KIBIS( type, *kibismodel, m_fields ) ) );

                wxCommandEvent dummyEvent;
                onIbisPinCombobox( dummyEvent );

                kibismodel = std::dynamic_pointer_cast<SIM_MODEL_KIBIS>(
                        m_libraryModels.at( m_modelNameCombobox->GetSelection() ) );
            }

            m_curModelType = type;
            break;
        }
    }

    m_curModelTypeOfDeviceType.at( deviceType ) = m_curModelType;
    updateWidgets();
}


template <typename T>
void DIALOG_SIM_MODEL<T>::onParamGridChanged( wxPropertyGridEvent& aEvent )
{
    if( IsIbisLoaded() )
    {
        wxString      propValue = "";
        wxString      propName = aEvent.GetPropertyName();
        wxPGProperty* property = aEvent.GetProperty();

        if( property && propName == "wftype" )
        {
            wxVariant variant = aEvent.GetPropertyValue();
            propValue = property->ValueToString( variant );

            std::shared_ptr<SIM_MODEL_KIBIS> kibismodel =
                    std::dynamic_pointer_cast<SIM_MODEL_KIBIS>(
                            m_libraryModels.at( m_modelNameCombobox->GetSelection() ) );

            kibismodel->SetParameters( std::string( propValue.c_str() ) );
            kibismodel->SetParamValue( "wftype", std::string( propValue.c_str() ) );
        }
    }
    updateWidgets();
}


template <typename T>
void DIALOG_SIM_MODEL<T>::onCodePreviewSetFocus( wxFocusEvent& aEvent )
{
    // For some reason all text gets selected for me -Mikolaj
    if( m_wasCodePreviewUpdated )
        m_codePreview->SelectNone();

    m_wasCodePreviewUpdated = false;
}


template <typename T>
void DIALOG_SIM_MODEL<T>::onPinAssignmentsGridCellChange( wxGridEvent& aEvent )
{
    int symbolPinIndex = aEvent.GetRow();
    int oldModelPinIndex = getModelPinIndex( aEvent.GetString() );
    int modelPinIndex = getModelPinIndex(
            m_pinAssignmentsGrid->GetCellValue( aEvent.GetRow(), aEvent.GetCol() ) );

    if( oldModelPinIndex != SIM_MODEL::PIN::NOT_CONNECTED )
        curModel().SetPinSymbolPinNumber( oldModelPinIndex, "" );

    if( modelPinIndex != SIM_MODEL::PIN::NOT_CONNECTED )
    {
        curModel().SetPinSymbolPinNumber( modelPinIndex,
            std::string( m_sortedSymbolPins.at( symbolPinIndex )->GetShownNumber().ToUTF8() ) );
    }

    updatePinAssignments();

    aEvent.Skip();
}


template <typename T>
void DIALOG_SIM_MODEL<T>::onPinAssignmentsGridSize( wxSizeEvent& aEvent )
{
    wxGridUpdateLocker deferRepaintsTillLeavingScope( m_pinAssignmentsGrid );

    int gridWidth = KIPLATFORM::UI::GetUnobscuredSize( m_pinAssignmentsGrid ).x;
    m_pinAssignmentsGrid->SetColSize( static_cast<int>( PIN_COLUMN::MODEL ), gridWidth / 2 );
    m_pinAssignmentsGrid->SetColSize( static_cast<int>( PIN_COLUMN::SYMBOL ), gridWidth / 2 );

    aEvent.Skip();
}


template <typename T>
void DIALOG_SIM_MODEL<T>::onExcludeSymbolCheckbox( wxCommandEvent& aEvent )
{
    curModel().SetIsEnabled( !m_excludeSymbolCheckbox->GetValue() );
}


template <typename T>
void DIALOG_SIM_MODEL<T>::onLibraryPathUpdate( wxUpdateUIEvent& aEvent )
{
    aEvent.Enable( m_useLibraryModelRadioButton->GetValue() );
}


template <typename T>
void DIALOG_SIM_MODEL<T>::onBrowseButtonUpdate( wxUpdateUIEvent& aEvent )
{
    aEvent.Enable( m_useLibraryModelRadioButton->GetValue() );
}


template <typename T>
void DIALOG_SIM_MODEL<T>::onModelNameComboboxUpdate( wxUpdateUIEvent& aEvent )
{
    aEvent.Enable( m_useLibraryModelRadioButton->GetValue() );
}


template <typename T>
void DIALOG_SIM_MODEL<T>::onOverrideCheckboxUpdate( wxUpdateUIEvent& aEvent )
{
    aEvent.Enable( m_useLibraryModelRadioButton->GetValue() );
}


template <typename T>
void DIALOG_SIM_MODEL<T>::onDeviceTypeChoiceUpdate( wxUpdateUIEvent& aEvent )
{
    aEvent.Enable( m_useInstanceModelRadioButton->GetValue() );
}


template <typename T>
void DIALOG_SIM_MODEL<T>::onTypeChoiceUpdate( wxUpdateUIEvent& aEvent )
{
    aEvent.Enable( m_useInstanceModelRadioButton->GetValue() || ( IsIbisLoaded() ) );
}


template <typename T>
void DIALOG_SIM_MODEL<T>::onParamGridSetFocus( wxFocusEvent& aEvent )
{
    // By default, when a property grid is focused, the textbox is not immediately focused until
    // Tab key is pressed. This is inconvenient, so we fix that here.

    wxPropertyGrid* grid = m_paramGrid->GetGrid();
    if( !grid )
    {
        wxFAIL;
        return;
    }

    wxPGProperty* selected = grid->GetSelection();

    if( !selected )
        selected = grid->wxPropertyGridInterface::GetFirst();

    if( selected )
        grid->DoSelectProperty( selected, wxPG_SEL_FOCUS );

    aEvent.Skip();
}


template <typename T>
void DIALOG_SIM_MODEL<T>::onParamGridSelectionChange( wxPropertyGridEvent& aEvent )
{
    wxPropertyGrid* grid = m_paramGrid->GetGrid();
    if( !grid )
    {
        wxFAIL;
        return;
    }

    // Jump over categories.
    if( grid->GetSelection() && grid->GetSelection()->IsCategory() )
    {
        wxPGProperty* selection = grid->GetSelection();

        // If the new selection is immediately above the previous selection, we jump up. Otherwise
        // we jump down. We do this by simulating up or down arrow keys.

        wxPropertyGridIterator it = grid->GetIterator( wxPG_ITERATE_VISIBLE, selection );
        it.Next();

        wxKeyEvent* keyEvent = new wxKeyEvent( wxEVT_KEY_DOWN );

        if( *it == m_prevParamGridSelection )
        {
            if( !selection->IsExpanded() )
            {
                grid->Expand( selection );
                keyEvent->m_keyCode = WXK_DOWN;
                wxQueueEvent( grid, keyEvent );

                // Does not work for some reason.
                /*m_paramGrid->DoSelectProperty( selection->Item( selection->GetChildCount() - 1 ),
                                               wxPG_SEL_FOCUS );*/
            }
            else
            {
                keyEvent->m_keyCode = WXK_UP;
                wxQueueEvent( grid, keyEvent );
            }
        }
        else
        {
            if( !selection->IsExpanded() )
                grid->Expand( selection );

            keyEvent->m_keyCode = WXK_DOWN;
            wxQueueEvent( grid, keyEvent );
        }

        m_prevParamGridSelection = grid->GetSelection();
        return;
    }

    wxWindow* editorControl = grid->GetEditorControl();
    if( !editorControl )
    {
        m_prevParamGridSelection = grid->GetSelection();
        return;
    }

    // Without this the user had to press tab before they could edit the field.
    editorControl->SetFocus();
    m_prevParamGridSelection = grid->GetSelection();
}

template <typename T>
void DIALOG_SIM_MODEL<T>::setIbisMode( bool aIbisMode )
{
    m_ibisModelCombobox->Show( aIbisMode );
    m_ibisPinCombobox->Show( aIbisMode );
    m_ibisModelLabel->Show( aIbisMode );
    m_ibisPinLabel->Show( aIbisMode );
    m_overrideCheckbox->Show( !aIbisMode );

    m_modelNameLabel->SetLabel( aIbisMode ? "Component:" : "Model:" );
    this->Fit();
    this->Refresh();
    this->Update();
}

template class DIALOG_SIM_MODEL<SCH_FIELD>;
template class DIALOG_SIM_MODEL<LIB_FIELD>;

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

#include <sim/sim_model_behavioral.h>

#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <fmt/core.h>


std::string SPICE_GENERATOR_BEHAVIORAL::ModelLine( const std::string& aModelName ) const
{
    return "";
}


std::string SPICE_GENERATOR_BEHAVIORAL::ItemLine( const std::string& aRefName,
                                                  const std::string& aModelName,
                                                  const std::vector<std::string>& aSymbolPinNumbers,
                                                  const std::vector<std::string>& aPinNetNames ) const
{
    switch( m_model.GetType() )
    {
    case SIM_MODEL::TYPE::R_BEHAVIORAL:
    case SIM_MODEL::TYPE::C_BEHAVIORAL:
    case SIM_MODEL::TYPE::L_BEHAVIORAL:
        return SPICE_GENERATOR::ItemLine( aRefName,
                                          m_model.GetParam( 0 ).value->ToString(),
                                          aSymbolPinNumbers,
                                          aPinNetNames );

    case SIM_MODEL::TYPE::V_BEHAVIORAL:
        return SPICE_GENERATOR::ItemLine( aRefName,
                                          fmt::format( "V={0}",
                                                       m_model.GetParam( 0 ).value->ToString() ),
                                          aSymbolPinNumbers,
                                          aPinNetNames );

    case SIM_MODEL::TYPE::I_BEHAVIORAL:
        return SPICE_GENERATOR::ItemLine( aRefName,
                                          fmt::format( "I={0}",
                                                       m_model.GetParam( 0 ).value->ToString() ),
                                          aSymbolPinNumbers,
                                          aPinNetNames );

    default:
        wxFAIL_MSG( "Unhandled SIM_MODEL type in SIM_MODEL_BEHAVIORAL" );
        return "";
    }
}


SIM_MODEL_BEHAVIORAL::SIM_MODEL_BEHAVIORAL( TYPE aType ) :
    SIM_MODEL( aType, std::make_unique<SPICE_GENERATOR_BEHAVIORAL>( *this ) ),
    m_isInferred( false )
{
    static PARAM::INFO resistor  = makeParams( "r", "Expression for resistance",  "Ω" );
    static PARAM::INFO capacitor = makeParams( "c", "Expression for capacitance", "F"   );
    static PARAM::INFO inductor  = makeParams( "l", "Expression for inductance",  "H"   );
    static PARAM::INFO vsource   = makeParams( "v", "Expression for voltage",     "V"   );
    static PARAM::INFO isource   = makeParams( "i", "Expression for current",     "A"   );

    switch( aType )
    {
    case TYPE::R_BEHAVIORAL: AddParam( resistor  ); break;
    case TYPE::C_BEHAVIORAL: AddParam( capacitor ); break;
    case TYPE::L_BEHAVIORAL: AddParam( inductor  ); break;
    case TYPE::V_BEHAVIORAL: AddParam( vsource   ); break;
    case TYPE::I_BEHAVIORAL: AddParam( isource   ); break;
    default:
        wxFAIL_MSG( "Unhandled SIM_MODEL type in SIM_MODEL_IDEAL" );
    }
}


void SIM_MODEL_BEHAVIORAL::ReadDataSchFields( unsigned aSymbolPinCount,
                                              const std::vector<SCH_FIELD>* aFields )
{
    if( GetFieldValue( aFields, PARAMS_FIELD ) != "" )
        SIM_MODEL::ReadDataSchFields( aSymbolPinCount, aFields );
    else
        inferredReadDataFields( aSymbolPinCount, aFields );
}


void SIM_MODEL_BEHAVIORAL::ReadDataLibFields( unsigned aSymbolPinCount,
                                              const std::vector<LIB_FIELD>* aFields )
{
    if( GetFieldValue( aFields, PARAMS_FIELD ) != "" )
        SIM_MODEL::ReadDataLibFields( aSymbolPinCount, aFields );
    else
        inferredReadDataFields( aSymbolPinCount, aFields );
}


void SIM_MODEL_BEHAVIORAL::WriteDataSchFields( std::vector<SCH_FIELD>& aFields ) const
{
    SIM_MODEL::WriteDataSchFields( aFields );

    if( m_isInferred )
        inferredWriteDataFields( aFields );
}


void SIM_MODEL_BEHAVIORAL::WriteDataLibFields( std::vector<LIB_FIELD>& aFields ) const
{
    SIM_MODEL::WriteDataLibFields( aFields );

    if( m_isInferred )
        inferredWriteDataFields( aFields );
}


bool SIM_MODEL_BEHAVIORAL::parseValueField( const std::string& aValueField )
{
    std::string expr = aValueField;

    if( expr.find( "=" ) == std::string::npos )
        return false;
    
    boost::replace_first( expr, "=", "" );

    SetParamValue( 0, boost::trim_copy( expr ) );
    return true;
}


template <typename T>
void SIM_MODEL_BEHAVIORAL::inferredReadDataFields( unsigned aSymbolPinCount,
                                                   const std::vector<T>* aFields )
{
    ParsePinsField( aSymbolPinCount, GetFieldValue( aFields, PINS_FIELD ) );

    if( ( InferTypeFromRefAndValue( GetFieldValue( aFields, REFERENCE_FIELD ),
                                    GetFieldValue( aFields, VALUE_FIELD ) ) == GetType()
            && parseValueField( GetFieldValue( aFields, VALUE_FIELD ) ) )
        // If Value is device type, this is an empty model
        || GetFieldValue( aFields, VALUE_FIELD ) == DeviceTypeInfo( GetDeviceType() ).fieldValue )
    {
        m_isInferred = true;
    }
}


template <typename T>
void SIM_MODEL_BEHAVIORAL::inferredWriteDataFields( std::vector<T>& aFields ) const
{
    std::string value = GetParam( 0 ).value->ToString();

    if( value == "" )
        value = GetDeviceTypeInfo().fieldValue;

    WriteInferredDataFields( aFields, "=" + value );
}


SIM_MODEL::PARAM::INFO SIM_MODEL_BEHAVIORAL::makeParams( std::string aName, std::string aDescription,
                                                         std::string aUnit )
{
    PARAM::INFO paramInfo = {};

    paramInfo.name = aName;
    paramInfo.type = SIM_VALUE::TYPE_STRING;
    paramInfo.unit = aUnit;
    paramInfo.category = PARAM::CATEGORY::PRINCIPAL;
    paramInfo.description = aDescription;

    return paramInfo;
}

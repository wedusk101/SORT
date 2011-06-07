/*
 * filename :	gridtexture.cpp
 *
 * programmer :	Cao Jiayin
 */

// include the header file
#include "gridtexture.h"

// default constructor
GridTexture::GridTexture():
	m_Color0( 1.0f , 1.0f , 1.0f ),
	m_Color1( 0.0f , 0.0f , 0.0f )
{
	_init();
}

// constructor from two colors
GridTexture::GridTexture( const Spectrum& c0 , const Spectrum& c1 )
{
	m_Color0 = c0;
	m_Color1 = c1;

	_init();
}

// constructor from six float
GridTexture::GridTexture( 	float r0 , float g0 , float b0 , 
							float r1 , float g1 , float b1 ):
	m_Color0( r0 , g0 , b0 ) , m_Color1( r1 , g1 , b1 )
{
	_init();
}

// destructor
GridTexture::~GridTexture()
{
}

// overwrite init method
void GridTexture::_init()
{
	// by default , the width and height if not zero
	// because making width and height none zero costs nothing
	// while makeing them zero forbids showing the texture
	m_iTexWidth = 16;
	m_iTexHeight = 16;
}

// get the color
const Spectrum& GridTexture::GetColor( int x , int y ) const 
{
	// filter the coorindate first
	_texCoordFilter( x , y );

	// return the color
	if( (( x - (int)m_iTexWidth / 2 ) * ( y - (int)m_iTexHeight /2 )) > 0 )
		return m_Color0;
	else
		return m_Color1;
}

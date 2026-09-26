#pragma once
#include <array>
#include <cmath>
namespace wxl_modern_m2::gilnean
{
using Matrix = std::array<float, 16>;
inline Matrix Multiply(const Matrix& a, const Matrix& b)
{
    Matrix out{};
    for (unsigned r=0;r<4;++r) for (unsigned c=0;c<4;++c)
        for (unsigned k=0;k<4;++k) out[r*4+c] += a[r*4+k]*b[k*4+c];
    return out;
}
inline bool Inverse(const Matrix& input, Matrix& out)
{
    double a[4][8]{};
    for (unsigned r=0;r<4;++r) for (unsigned c=0;c<4;++c)
    { a[r][c]=input[r*4+c]; a[r][c+4]=r==c; }
    for (unsigned c=0;c<4;++c)
    {
        unsigned pivot=c;
        for (unsigned r=c+1;r<4;++r) if (std::abs(a[r][c])>std::abs(a[pivot][c])) pivot=r;
        if (!std::isfinite(a[pivot][c]) || std::abs(a[pivot][c])<1e-10) return false;
        for (unsigned j=0;j<8;++j) { double v=a[c][j];a[c][j]=a[pivot][j];a[pivot][j]=v; }
        const double divisor=a[c][c];
        for (double& v:a[c]) v/=divisor;
        for (unsigned r=0;r<4;++r) if (r!=c)
        { const double f=a[r][c]; for (unsigned j=0;j<8;++j) a[r][j]-=f*a[c][j]; }
    }
    for (unsigned r=0;r<4;++r) for (unsigned c=0;c<4;++c) out[r*4+c]=float(a[r][c+4]);
    return true;
}
inline Matrix Side(const Matrix& world, const Matrix& inverseView)
{
    Matrix out=world;
    for (unsigned i=0;i<12;++i) out[i]*=0.62f;
    // Camera right projected onto the floor: negative is screen-left, independent
    // of character facing. Keep the shared ground height exactly unchanged.
    const float length=std::hypot(inverseView[0],inverseView[1]);
    if (length>0.0001f)
    { out[12]-=1.65f*inverseView[0]/length;out[13]-=1.65f*inverseView[1]/length; }
    return out;
}
}

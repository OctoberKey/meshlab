#include "filter_sdfgpu.h"
#include <vcg/complex/trimesh/base.h>
#include <vcg/complex/trimesh/update/topology.h>
#include <vcg/complex/trimesh/update/edges.h>
#include <vcg/complex/trimesh/update/bounding.h>
#include <vcg/complex/trimesh/update/quality.h>
#include <vcg/complex/trimesh/update/color.h>
#include <vcg/complex/trimesh/update/flag.h>
#include <vcg/complex/trimesh/clean.h>
#include <vcg/complex/intersection.h>
#include <vcg/space/index/grid_static_ptr.h>
#include <vcg/space/index/spatial_hashing.h>
#include <vcg/math/matrix33.h>
#include <wrap/qt/to_string.h>
#include <GL/glew.h>
#include <stdio.h>
using namespace std;
using namespace vcg;



SdfGpuPlugin::SdfGpuPlugin() : SingleMeshFilterInterface("Compute SDF GPU"), shaderProgram(-1){}

void SdfGpuPlugin::initParameterSet(MeshDocument&, RichParameterSet& par){
  qDebug() << "called here!";
  QStringList onPrimitive; onPrimitive.push_back("On vertices"); onPrimitive.push_back("On Faces");
  par.addParam( new RichEnum("onPrimitive", 0, onPrimitive, "Metric:",
                             "Choose whether to trace rays from faces or from vertices. "
                             "Recall that tracing from vertices will use vertex normal "
                             "estimation."));
  par.addParam(new RichFloat("coneWidth", 0.001, "Cone width: ",
                             "The standard deviation of the rays that will be casted around "
                             "the anti-normals. Remember that most sampled directions are "
                             "expected to fall within 3x this value."));
  par.addParam(  new RichInt("numberRays", 10, "Number of rays: ",
                             "The standard deviation of the rays that will be casted around "
                             "the anti-normals. Remember that most sampled directions are "
                             "expected to fall within 3x this value."));
  par.addParam(new RichFloat("lowQuantile", .1, "Bottom quantile",
                             "We will throw away the set of ray distances for each cone which distance "
                             "value falls under this quantile. Value in between [0,1]. 0 Implies all "
                             "values are kept"));
  par.addParam(new RichFloat("hiQuantile", .9, "Top quantile",
                             "We will throw away the set of ray distances for each cone which distance "
                             "value falls under this quantile. Value in between [0,1]. 1 Implies all "
                             "values are kept"));
}


bool SdfGpuPlugin::applyFilter(MeshDocument& md, RichParameterSet& pars, vcg::CallBackPos* cb){


  //--- Retrieve parameters
  float widenessRad       = math::ToRad(pars.getFloat("coneWidth"));
  int   raysPerCone       = pars.getInt("numberRays");
  ONPRIMITIVE onPrimitive = (ONPRIMITIVE) pars.getInt("onPrimitive");
  float lo01pec           = pars.getFloat("lowQuantile");
  float hi01pec           = pars.getFloat("hiQuantile");

  assert( onPrimitive==ON_VERTICES && "Face mode not supported yet" );

  //---GL init
  initGL();

  setupMesh( md, onPrimitive );

  assert(checkAndLogGL() && "after init");

  int   peel = 3;
  float tolerance = 0.0005f;

  for( int i=0;  i < peel; i++ )
  {
      if( i==0 )
            useDefaultShader();
      else
      {
            vsB->useAsSource();
            useDepthPeelingShader();
            setDepthPeelingTolerance(tolerance);
            setDepthPeelingSize(*vsB);
      }

      vsA->useAsDest();

      fillFrameBuffer(i%2==0,md.mm());

      std::swap<Vscan*>(vsA,vsB);

      checkAndLogGL();
 }

  assert(checkAndLogGL() && "After deepth peeling");

  releaseGL();

 // assert(checkAndLogGL() && "After release"); ?? oerazione non valida ma funziona??


/* for(int i=0; i<md.mm()->cm.vert.size(); i++)
 {
     CVertexO& v = md.mm()->cm.vert[i];

     //--- Save in mesh
     v.Q() = 1;
 }*/

  return true;
}

bool SdfGpuPlugin::checkAndLogGL()
{
   GLenum err = glGetError();

   if(err != GL_NO_ERROR)
   {
        const GLubyte* errname = gluErrorString(err);
        Log((const char*)errname);
        return false;
   }

   return true;
}

void SdfGpuPlugin::initGL()
{
    this->glContext->makeCurrent();

    vsA = new Vscan(128, 128);
    vsB = new Vscan(128, 128);

    vsA->init();
    vsB->init();

    glClearColor(1,0,0,1);
    glClear(GL_COLOR_BUFFER_BIT);
}

void SdfGpuPlugin::releaseGL()
{
    useDefaultShader();
    useScreenAsDest();

    delete vsA;
    delete vsB;

    this->glContext->doneCurrent();
}

void SdfGpuPlugin::fillFrameBuffer(bool front,  MeshModel* mm)
{
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    glEnable(GL_CULL_FACE);
    glCullFace((front)?GL_BACK:GL_FRONT);

    glPushMatrix();

    mm->glw.Draw<GLW::DMSmooth, GLW::CMNone, GLW::TMNone>();

    glPopMatrix();
}

void SdfGpuPlugin::setupMesh(MeshDocument& md, ONPRIMITIVE onPrimitive )
{
    MeshModel* mm = md.mm();
    CMeshO& m     = mm->cm;


    //--- Use VBO for faster drawing ?? si arrabbia
   /* mm->glw.SetHint(vcg::GLW::HNUseVBO);
    mm->glw.Update();*/

    //--- If on vertices, do some cleaning first
    if( onPrimitive == ON_VERTICES )
    {
      int dup = tri::Clean<CMeshO>::RemoveDuplicateVertex(m);
      int unref =  tri::Clean<CMeshO>::RemoveUnreferencedVertex(m);
      if (dup > 0 || unref > 0) Log("Removed %i duplicate and %i unreferenced vertices\n",dup,unref);
    }

    //--- Updating mesh metadata
    tri::UpdateBounding<CMeshO>::Box(m);
    tri::UpdateNormals<CMeshO>::PerFaceNormalized(m);
    tri::UpdateNormals<CMeshO>::PerVertexAngleWeighted(m);
    tri::UpdateNormals<CMeshO>::NormalizeVertex(m);
    tri::UpdateFlags<CMeshO>::FaceProjection(m);

    //--- Enable & Reset the necessary attributes
    switch(onPrimitive){
      case ON_VERTICES:
        mm->updateDataMask(MeshModel::MM_VERTQUALITY);
        tri::UpdateQuality<CMeshO>::VertexConstant(m,0);
        break;
      case ON_FACES:
        mm->updateDataMask(MeshModel::MM_FACEQUALITY);
        tri::UpdateQuality<CMeshO>::FaceConstant(m,0);
        break;
    }


}

void SdfGpuPlugin::useDepthPeelingShader()
{
    if ( shaderProgram == -1 )
    {
        QDir shadersDir = QDir(qApp->applicationDirPath());

    #if defined(Q_OS_WIN)
        if (shadersDir.dirName() == "debug" || shadersDir.dirName() == "release" || shadersDir.dirName() == "plugins"  )
                shadersDir.cdUp();
    #elif defined(Q_OS_MAC)
        if (shadersDir.dirName() == "MacOS") {
                for(int i=0;i<4;++i)
                {
                        if(shadersDir.exists("shaders"))
                                break;
                        shadersDir.cdUp();
                }
        }
    #endif

        bool ret=shadersDir.cd("shaders");
        if(!ret)
        {
                QMessageBox::information(0, "Sdf Gpu Plugin","Unable to find the shaders directory.\nNo shaders will be loaded.");
                return;
        }

        QString    fileName("shaderDepthPeeling.fs");
        QByteArray ba;
        QFile      file;
        char*      data;
        file.setFileName(shadersDir.absoluteFilePath(fileName));

        int f = glCreateShader(GL_FRAGMENT_SHADER);
        if (file.open(QIODevice::ReadOnly))
        {
                QTextStream ts(&file);
                ba = ts.readAll().toLocal8Bit();
                data = ba.data();
                glShaderSource(f, 1, (const GLchar**)&data,NULL);
                glCompileShader(f);
                GLint errF;
                glGetShaderiv(f,GL_COMPILE_STATUS,&errF);
                assert(errF==GL_TRUE);
                file.close();
        }
        // init shaders

        shaderProgram = glCreateProgram();
        glAttachShader(shaderProgram,f);
        glLinkProgram(shaderProgram);
   }

   glUseProgram(shaderProgram);

   glUniform1i( glGetUniformLocation(shaderProgram,"textureLastDepth"), 0 );

   float m[16];
   glUniformMatrix4fv(glGetUniformLocation(shaderProgram,"matr"), 1,1, m );
}

Q_EXPORT_PLUGIN(SdfGpuPlugin)
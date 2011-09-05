#include "PlusConfigure.h"

#include <limits>

#include "vtkBMPWriter.h"
#include "vtkImageImport.h" 
#include "vtkImageData.h" 
#include "vtkImageViewer.h"
#include "vtkObjectFactory.h"
#include "vtkSmartPointer.h"
#include "vtkTimerLog.h"
#include "vtkTrackerTool.h"
#include "vtkTrackerBuffer.h"
#include "vtkTransform.h"
#include "vtkVideoBuffer.h"
#include "vtkVolumeReconstructor.h"
#include "vtkXMLUtilities.h"
#include "vtkImageExtractComponents.h"

#include "vtkVolumeReconstructorFilter.h"
#include "vtkTrackedFrameList.h"

vtkCxxRevisionMacro(vtkVolumeReconstructor, "$Revisions: 1.0 $");
vtkStandardNewMacro(vtkVolumeReconstructor);

//----------------------------------------------------------------------------
vtkVolumeReconstructor::vtkVolumeReconstructor()
{
  this->Reconstructor = vtkVolumeReconstructorFilter::New();  
  this->ImageToToolTransform = vtkTransform::New();
}

//----------------------------------------------------------------------------
vtkVolumeReconstructor::~vtkVolumeReconstructor()
{
  if (this->Reconstructor!=NULL)
  {
    this->Reconstructor->Delete();
    this->Reconstructor=NULL;
  }
  if (this->ImageToToolTransform!=NULL)
  {
    this->ImageToToolTransform->Delete();
    this->ImageToToolTransform=NULL;
  }
}

//----------------------------------------------------------------------------
void vtkVolumeReconstructor::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os,indent);
}

//----------------------------------------------------------------------------
void vtkVolumeReconstructor::FillHoles()
{
  this->Reconstructor->FillHolesInOutput(); 
}

//----------------------------------------------------------------------------
PlusStatus vtkVolumeReconstructor::ReadConfiguration(vtkXMLDataElement* config)
{
  // Read reconstruction parameters
  this->Reconstructor->ReadConfiguration(config); 

  // Read calibration matrix (ImageToTool transform)
  vtkSmartPointer<vtkXMLDataElement> dataCollectionConfig = config->FindNestedElementWithName("USDataCollection");
  if (dataCollectionConfig == NULL)
  {
    LOG_ERROR("Cannot find USDataCollection element in XML tree!");
    return PLUS_FAIL;
  }
  vtkSmartPointer<vtkXMLDataElement> trackerDefinition = dataCollectionConfig->FindNestedElementWithName("Tracker"); 
  if ( trackerDefinition == NULL) 
  {
    LOG_ERROR("Cannot find Tracker element in XML tree!");
    return PLUS_FAIL;
  }
  std::string toolType;
  vtkTracker::ConvertToolTypeToString(TRACKER_TOOL_PROBE, toolType);
  vtkSmartPointer<vtkXMLDataElement> probeDefinition = trackerDefinition->FindNestedElementWithNameAndAttribute("Tool", "Type", toolType.c_str());
  if (probeDefinition == NULL) {
    LOG_ERROR("No probe definition is found in the XML tree!");
    return PLUS_FAIL;
  }
  vtkSmartPointer<vtkXMLDataElement> calibration = probeDefinition->FindNestedElementWithName("Calibration");
  if (calibration == NULL) {
    LOG_ERROR("No calibration section is found in probe definition!");
    return PLUS_FAIL;
  }
  double aImageToTool[16];
  if (!calibration->GetVectorAttribute("MatrixValue", 16, aImageToTool)) 
  {
    LOG_ERROR("No calibration matrix is found in probe definition!");
    return PLUS_FAIL;
  }

  this->ImageToToolTransform->SetMatrix( aImageToTool );

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
vtkTransform* vtkVolumeReconstructor::GetImageToToolTransform()
{ 
  return this->ImageToToolTransform;
}

//----------------------------------------------------------------------------
void vtkVolumeReconstructor::GetImageToReferenceTransformMatrix(vtkMatrix4x4* toolToReferenceTransformMatrix, vtkMatrix4x4* imageToReferenceTransformMatrix)
{
  // Transformation chain: ImageToReference = ToolToReference * ImageToTool
  vtkMatrix4x4::Multiply4x4(
    toolToReferenceTransformMatrix, this->ImageToToolTransform->GetMatrix(),
    imageToReferenceTransformMatrix);  
}

//----------------------------------------------------------------------------
PlusStatus vtkVolumeReconstructor::GetImageToReferenceTransformMatrix(TrackedFrame* frame, vtkMatrix4x4* imageToReferenceTransformMatrix)
{
  double defaultTransform[16]; 
  if ( !frame->GetDefaultFrameTransform(defaultTransform) )		
  {
    LOG_ERROR("Unable to get default frame transform"); 
    return PLUS_FAIL; 
  }
  vtkSmartPointer<vtkMatrix4x4> toolToReferenceTransformMatrix=vtkSmartPointer<vtkMatrix4x4>::New();
  toolToReferenceTransformMatrix->DeepCopy(defaultTransform);

  GetImageToReferenceTransformMatrix(toolToReferenceTransformMatrix, imageToReferenceTransformMatrix);
  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
void vtkVolumeReconstructor::AddImageToExtent( vtkImageData *image, vtkMatrix4x4* mImageToReference, double* extent_Ref)
{
  // Output volume is in the Reference coordinate system.

  // Prepare the four corner points of the input US image.
  int* frameExtent=image->GetExtent();
  std::vector< double* > corners_ImagePix;
  double c0[ 4 ] = { frameExtent[ 0 ], frameExtent[ 2 ], 0,  1 };
  double c1[ 4 ] = { frameExtent[ 0 ], frameExtent[ 3 ], 0,  1 };
  double c2[ 4 ] = { frameExtent[ 1 ], frameExtent[ 2 ], 0,  1 };
  double c3[ 4 ] = { frameExtent[ 1 ], frameExtent[ 3 ], 0,  1 };
  corners_ImagePix.push_back( c0 );
  corners_ImagePix.push_back( c1 );
  corners_ImagePix.push_back( c2 );
  corners_ImagePix.push_back( c3 );

  // Transform the corners to Reference and expand the extent if needed
  for ( unsigned int corner = 0; corner < corners_ImagePix.size(); ++corner )
  {
    double corner_Ref[ 4 ] = { 0, 0, 0, 1 }; // position of the corner in the Reference coordinate system
    mImageToReference->MultiplyPoint( corners_ImagePix[corner], corner_Ref );

    for ( int axis = 0; axis < 3; axis ++ )
    {
      if ( corner_Ref[axis] < extent_Ref[axis*2] )
      {
        // min extent along this coord axis has to be decreased
        extent_Ref[axis*2]=corner_Ref[axis];
      }
      if ( corner_Ref[axis] > extent_Ref[axis*2+1] )
      {
        // max extent along this coord axis has to be increased
        extent_Ref[axis*2+1]=corner_Ref[axis];
      }
    }
  }
} 

//----------------------------------------------------------------------------
PlusStatus vtkVolumeReconstructor::SetOutputExtentFromFrameList(vtkTrackedFrameList* trackedFrameList)
{
  double extent_Ref[6]=
  {
    VTK_DOUBLE_MAX, VTK_DOUBLE_MIN,
    VTK_DOUBLE_MAX, VTK_DOUBLE_MIN,
    VTK_DOUBLE_MAX, VTK_DOUBLE_MIN
  };

  const int numberOfFrames = trackedFrameList->GetNumberOfTrackedFrames(); 
  for (int frameIndex = 0; frameIndex < numberOfFrames; ++frameIndex )
  {
    TrackedFrame* frame = trackedFrameList->GetTrackedFrame( frameIndex );

    // Get transform
    vtkSmartPointer<vtkMatrix4x4> imageToReferenceTransformMatrix=vtkSmartPointer<vtkMatrix4x4>::New();
    if ( GetImageToReferenceTransformMatrix(frame, imageToReferenceTransformMatrix)!=PLUS_SUCCESS )		
    {
      LOG_ERROR("Unable to get image to reference transform for frame #" << frameIndex); 
      continue; 
    }

    // Get image (only the frame extents are needed)
    vtkImageData* frameImage=trackedFrameList->GetTrackedFrame(frameIndex)->ImageData.GetVtkImageNonFlipped();

    // Expand the extent_Ref to include this frame
    AddImageToExtent(frameImage, imageToReferenceTransformMatrix, extent_Ref);
  }

  // Set the output extent from the current min and max values, using the user-defined image resolution.

  int outputExtent[ 6 ] = { 0, 0, 0, 0, 0, 0 };
  double* outputSpacing = this->Reconstructor->GetOutputSpacing();
  outputExtent[ 1 ] = int( ( extent_Ref[1] - extent_Ref[0] ) / outputSpacing[ 0 ] );
  outputExtent[ 3 ] = int( ( extent_Ref[3] - extent_Ref[2] ) / outputSpacing[ 1 ] );
  outputExtent[ 5 ] = int( ( extent_Ref[5] - extent_Ref[4] ) / outputSpacing[ 2 ] );

  this->Reconstructor->SetOutputExtent( outputExtent );
  this->Reconstructor->SetOutputOrigin( extent_Ref[0], extent_Ref[2], extent_Ref[4] ); 
  try
  {
    if (this->Reconstructor->ResetOutput()!=PLUS_SUCCESS) // :TODO: call this automatically
    {
      LOG_ERROR("Failed to initialize output of the reconstructor");
      return PLUS_FAIL;
    }
  }
  catch(vtkstd::bad_alloc& e)
  {
    cerr << e.what() << endl;
    LOG_ERROR("StartReconstruction failed with due to out of memory. Try to reduce the size or spacing of the output volume.");
    return PLUS_FAIL;
  }

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkVolumeReconstructor::AddTrackedFrame(TrackedFrame* frame)
{
  vtkSmartPointer<vtkMatrix4x4> imageToReferenceTransformMatrix=vtkSmartPointer<vtkMatrix4x4>::New();
  if ( GetImageToReferenceTransformMatrix(frame, imageToReferenceTransformMatrix)!=PLUS_SUCCESS )		
  {
    LOG_ERROR("Unable to get image to reference transform for frame"); 
    return PLUS_FAIL; 
  }

  vtkImageData* frameImage=frame->ImageData.GetVtkImageNonFlipped();

  return this->Reconstructor->InsertSlice(frameImage, imageToReferenceTransformMatrix);
}

//----------------------------------------------------------------------------
PlusStatus vtkVolumeReconstructor::GetReconstructedVolume(vtkImageData* reconstructedVolume)
{
  vtkSmartPointer<vtkImageExtractComponents> extract = vtkSmartPointer<vtkImageExtractComponents>::New();
  
  // keep only 0th component (the other component is the mask that shows which voxels were pasted from slices)
  extract->SetComponents(0);
  extract->SetInput(this->Reconstructor->GetReconstructedVolume());
  extract->Update();
  reconstructedVolume->DeepCopy(extract->GetOutput());
  return PLUS_SUCCESS;
}

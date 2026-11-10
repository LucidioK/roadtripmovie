
[CmdletBinding()]
param (
    [Parameter(Mandatory = $true, ValueFromPipeline = $true, ValueFromPipelineByPropertyName = $true)]
    [string]$ImagePath
)

process {
    # Resolve absolute path to handle relative paths correctly
    $FullImgPath = Resolve-Path $ImagePath -ErrorAction SilentlyContinue;
    if (-not $FullImgPath) {
        Write-Error "File not found: $ImagePath";
        return;
    }

    try {
        # Instantiate the native Windows Image Acquisition (WIA) COM object
        $WiaImage = New-Object -ComObject Wia.ImageFile;
        $WiaImage.LoadFile($FullImgPath.Path);

        # Validate if GPS metadata keys exist
        if (-not ($WiaImage.Properties.Exists('GpsLatitude') -and $WiaImage.Properties.Exists('GpsLongitude'))) {
            Write-Warning "No GPS metadata found in: $(Split-Path $FullImgPath -Leaf)";
            return;
        }

        function Get-GoogleMapsUrl ($LatProp, $LatRef, $LongProp, $LongRef) {
            $LatDeg = $LatProp.Value[1].Value;
            $LatMin = $LatProp.Value[2].Value;
            $LatSec = $LatProp.Value[3].Value;
            $LongDeg = $LongProp.Value[1].Value;
            $LongMin = $LongProp.Value[2].Value;
            $LongSec = $LongProp.Value[3].Value;
            return "https://www.google.com/maps/place/$LatDeg%C2%B0$LatMin'$LatSec%22$LatRef+$LongDeg%C2%B0$LongMin'$LongSec%22$LongRef";
        }
        # Helper function to convert rational EXIF arrays to Decimal Degrees
        function Convert-ToDecimalDegrees ($Property, $Ref) {
            # EXIF storing formats: element [1] = Degrees, [2] = Minutes, [3] = Seconds
            $Deg = $Property.Value[1].Value;
            $Min = $Property.Value[2].Value;
            $Sec = $Property.Value[3].Value;

            $Decimal = $Deg + ($Min / 60) + ($Sec / 3600);

            # Negate value if South or West hemisphere
            if ($Ref -match 'S|W') {
                $Decimal = -$Decimal;
            }
            return [Math]::Round($Decimal, 6);
        }

        # Gather property properties
        $LatProp  = $WiaImage.Properties.Item('GpsLatitude');
        $LatRef   = $WiaImage.Properties.Item('GpsLatitudeRef').Value;
        $LongProp = $WiaImage.Properties.Item('GpsLongitude');
        $LongRef  = $WiaImage.Properties.Item('GpsLongitudeRef').Value;

        # Calculate decimals
        $Latitude  = Convert-ToDecimalDegrees $LatProp $LatRef;
        $Longitude = Convert-ToDecimalDegrees $LongProp $LongRef;

        # Output as a clean custom object
        [PSCustomObject]@{
            FileName  = Split-Path $FullImgPath -Leaf;
            Latitude  = $Latitude;
            Longitude = $Longitude;
            GoogleMapsUrl = Get-GoogleMapsUrl $LatProp $LatRef $LongProp $LongRef;
        }
    }
    catch {
        Write-Error "Failed to process image metadata: $_";
    }
}


# --- How to use it ---
# Single file run:
# Get-ImageCoordinates -ImagePath "C:\path\to\your\photo.jpg"

# Batch process an entire directory:
# Get-ChildItem "C:\path\to\images\*.jpg" | Get-ImageCoordinates | Out-GridView

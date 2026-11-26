# Measured rotation of the physical workspace relative to the kinematic model.
$workspaceRotationDeg = 1.18

function Get-WrappedDeltaCounts([long]$From, [long]$To) {
    $delta = ($To - $From) % 36000
    if ($delta -ge 18000) { $delta -= 36000 }
    if ($delta -lt -18000) { $delta += 36000 }
    return $delta
}
